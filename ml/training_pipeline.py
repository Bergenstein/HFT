"""
Training Pipeline for LSTM Trading Model
Handles data loading, model training, validation, and checkpointing
"""

import os
import json
import time
from datetime import datetime
from typing import Dict, Optional, Tuple, List
from dataclasses import dataclass, asdict
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader
from tqdm import tqdm

import sys
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/models')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/features')

from lstm_model import create_model, LSTMTradingModel
from feature_extractor import FeatureExtractor, FeatureConfig


@dataclass
class TrainingConfig:
    """Training configuration"""
    # Model
    model_type: str = 'lstm'
    hidden_size: int = 128
    num_layers: int = 2
    dropout: float = 0.2
    bidirectional: bool = True
    use_attention: bool = True
    
    # Training
    batch_size: int = 64
    learning_rate: float = 1e-3
    weight_decay: float = 1e-5
    num_epochs: int = 100
    early_stopping_patience: int = 10
    gradient_clip: float = 1.0
    
    # Data
    sequence_length: int = 50
    train_ratio: float = 0.7
    val_ratio: float = 0.15
    
    # Output
    output_type: str = 'regression'  # 'regression', 'classification', 'binary'
    
    # Device
    device: str = 'cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu'


class TradingDataset(Dataset):
    """PyTorch Dataset for trading data"""
    
    def __init__(self, X: np.ndarray, y: np.ndarray):
        self.X = torch.FloatTensor(X)
        self.y = torch.FloatTensor(y)
    
    def __len__(self):
        return len(self.X)
    
    def __getitem__(self, idx):
        return self.X[idx], self.y[idx]


class EarlyStopping:
    """Early stopping to prevent overfitting"""
    
    def __init__(self, patience: int = 10, min_delta: float = 1e-6):
        self.patience = patience
        self.min_delta = min_delta
        self.counter = 0
        self.best_loss = float('inf')
        self.should_stop = False
    
    def __call__(self, val_loss: float) -> bool:
        if val_loss < self.best_loss - self.min_delta:
            self.best_loss = val_loss
            self.counter = 0
        else:
            self.counter += 1
            if self.counter >= self.patience:
                self.should_stop = True
        return self.should_stop


class Trainer:
    """
    Trainer class for LSTM trading models
    """
    
    def __init__(
        self,
        model: nn.Module,
        config: TrainingConfig,
        save_dir: str = '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/models/checkpoints'
    ):
        self.model = model
        self.config = config
        self.save_dir = save_dir
        self.device = torch.device(config.device)
        
        # Move model to device
        self.model = self.model.to(self.device)
        
        # Loss function
        if config.output_type == 'regression':
            self.criterion = nn.MSELoss()
        elif config.output_type == 'classification':
            self.criterion = nn.CrossEntropyLoss()
        elif config.output_type == 'binary':
            self.criterion = nn.BCELoss()
        
        # Optimizer
        self.optimizer = optim.AdamW(
            self.model.parameters(),
            lr=config.learning_rate,
            weight_decay=config.weight_decay
        )
        
        # Learning rate scheduler
        self.scheduler = optim.lr_scheduler.ReduceLROnPlateau(
            self.optimizer,
            mode='min',
            factor=0.5,
            patience=5
        )
        
        # Training history
        self.history = {
            'train_loss': [],
            'val_loss': [],
            'learning_rate': []
        }
        
        # Create save directory
        os.makedirs(save_dir, exist_ok=True)
    
    def train_epoch(self, train_loader: DataLoader) -> float:
        """Train for one epoch"""
        self.model.train()
        total_loss = 0.0
        num_batches = 0
        
        for X_batch, y_batch in train_loader:
            X_batch = X_batch.to(self.device)
            y_batch = y_batch.to(self.device)
            
            # Forward pass
            self.optimizer.zero_grad()
            output = self.model(X_batch)
            
            # Compute loss
            if self.config.output_type == 'regression':
                loss = self.criterion(output.squeeze(), y_batch)
            else:
                loss = self.criterion(output, y_batch.long())
            
            # Backward pass
            loss.backward()
            
            # Gradient clipping
            if self.config.gradient_clip:
                torch.nn.utils.clip_grad_norm_(
                    self.model.parameters(), 
                    self.config.gradient_clip
                )
            
            self.optimizer.step()
            
            total_loss += loss.item()
            num_batches += 1
        
        return total_loss / num_batches
    
    def validate(self, val_loader: DataLoader) -> Tuple[float, Dict]:
        """Validate the model"""
        self.model.eval()
        total_loss = 0.0
        num_batches = 0
        
        all_preds = []
        all_targets = []
        
        with torch.no_grad():
            for X_batch, y_batch in val_loader:
                X_batch = X_batch.to(self.device)
                y_batch = y_batch.to(self.device)
                
                output = self.model(X_batch)
                
                if self.config.output_type == 'regression':
                    loss = self.criterion(output.squeeze(), y_batch)
                    all_preds.extend(output.squeeze().cpu().numpy())
                else:
                    loss = self.criterion(output, y_batch.long())
                    all_preds.extend(torch.argmax(output, dim=1).cpu().numpy())
                
                all_targets.extend(y_batch.cpu().numpy())
                
                total_loss += loss.item()
                num_batches += 1
        
        avg_loss = total_loss / num_batches
        
        # Compute metrics
        metrics = self._compute_metrics(
            np.array(all_preds), 
            np.array(all_targets)
        )
        
        return avg_loss, metrics
    
    def _compute_metrics(self, preds: np.ndarray, targets: np.ndarray) -> Dict:
        """Compute evaluation metrics"""
        metrics = {}
        
        if self.config.output_type == 'regression':
            # Regression metrics
            mse = np.mean((preds - targets) ** 2)
            mae = np.mean(np.abs(preds - targets))
            
            # Direction accuracy
            pred_direction = np.sign(preds)
            target_direction = np.sign(targets)
            direction_accuracy = np.mean(pred_direction == target_direction)
            
            # Correlation
            if np.std(preds) > 0 and np.std(targets) > 0:
                correlation = np.corrcoef(preds, targets)[0, 1]
            else:
                correlation = 0.0
            
            metrics = {
                'mse': float(mse),
                'rmse': float(np.sqrt(mse)),
                'mae': float(mae),
                'direction_accuracy': float(direction_accuracy),
                'correlation': float(correlation)
            }
        else:
            # Classification metrics
            accuracy = np.mean(preds == targets)
            metrics = {
                'accuracy': float(accuracy)
            }
        
        return metrics
    
    def train(
        self,
        train_data: Dict,
        verbose: bool = True
    ) -> Dict:
        """
        Full training loop
        
        Args:
            train_data: Dictionary with X_train, y_train, X_val, y_val arrays
            verbose: Whether to print progress
        
        Returns:
            Training history and best metrics
        """
        # Create data loaders
        train_dataset = TradingDataset(train_data['X_train'], train_data['y_train'])
        val_dataset = TradingDataset(train_data['X_val'], train_data['y_val'])
        
        train_loader = DataLoader(
            train_dataset,
            batch_size=self.config.batch_size,
            shuffle=True,
            num_workers=0
        )
        val_loader = DataLoader(
            val_dataset,
            batch_size=self.config.batch_size,
            shuffle=False,
            num_workers=0
        )
        
        # Early stopping
        early_stopping = EarlyStopping(patience=self.config.early_stopping_patience)
        
        best_val_loss = float('inf')
        best_metrics = {}
        best_epoch = 0
        
        # Training loop
        iterator = tqdm(range(self.config.num_epochs), desc="Training") if verbose else range(self.config.num_epochs)
        
        for epoch in iterator:
            # Train
            train_loss = self.train_epoch(train_loader)
            
            # Validate
            val_loss, val_metrics = self.validate(val_loader)
            
            # Update scheduler
            self.scheduler.step(val_loss)
            
            # Record history
            current_lr = self.optimizer.param_groups[0]['lr']
            self.history['train_loss'].append(train_loss)
            self.history['val_loss'].append(val_loss)
            self.history['learning_rate'].append(current_lr)
            
            # Check for best model
            if val_loss < best_val_loss:
                best_val_loss = val_loss
                best_metrics = val_metrics
                best_epoch = epoch
                self.save_checkpoint('best_model.pt', epoch, val_loss, val_metrics)
            
            # Progress update
            if verbose:
                metrics_str = ', '.join([f"{k}: {v:.4f}" for k, v in val_metrics.items()])
                iterator.set_postfix({
                    'train_loss': f'{train_loss:.4f}',
                    'val_loss': f'{val_loss:.4f}',
                    'best': f'{best_val_loss:.4f}'
                })
            
            # Early stopping check
            if early_stopping(val_loss):
                if verbose:
                    print(f"\nEarly stopping at epoch {epoch}")
                break
        
        # Save final model
        self.save_checkpoint('final_model.pt', epoch, val_loss, val_metrics)
        
        # Load best model
        self.load_checkpoint('best_model.pt')
        
        return {
            'history': self.history,
            'best_val_loss': best_val_loss,
            'best_metrics': best_metrics,
            'best_epoch': best_epoch,
            'total_epochs': epoch + 1
        }
    
    def save_checkpoint(
        self,
        filename: str,
        epoch: int,
        val_loss: float,
        metrics: Dict
    ):
        """Save model checkpoint"""
        checkpoint = {
            'epoch': epoch,
            'model_state_dict': self.model.state_dict(),
            'optimizer_state_dict': self.optimizer.state_dict(),
            'val_loss': val_loss,
            'metrics': metrics,
            'config': asdict(self.config)
        }
        path = os.path.join(self.save_dir, filename)
        torch.save(checkpoint, path)
    
    def load_checkpoint(self, filename: str):
        """Load model checkpoint"""
        path = os.path.join(self.save_dir, filename)
        if os.path.exists(path):
            checkpoint = torch.load(path, map_location=self.device)
            self.model.load_state_dict(checkpoint['model_state_dict'])
            self.optimizer.load_state_dict(checkpoint['optimizer_state_dict'])
            return checkpoint
        return None
    
    def evaluate(self, test_data: Dict) -> Dict:
        """Evaluate on test set"""
        test_dataset = TradingDataset(test_data['X_test'], test_data['y_test'])
        test_loader = DataLoader(
            test_dataset,
            batch_size=self.config.batch_size,
            shuffle=False
        )
        
        test_loss, test_metrics = self.validate(test_loader)
        
        return {
            'test_loss': test_loss,
            'test_metrics': test_metrics
        }
    
    def predict(self, X: np.ndarray) -> np.ndarray:
        """Make predictions"""
        self.model.eval()
        X_tensor = torch.FloatTensor(X).to(self.device)
        
        with torch.no_grad():
            output = self.model(X_tensor)
            if self.config.output_type == 'classification':
                return torch.argmax(output, dim=1).cpu().numpy()
            return output.squeeze().cpu().numpy()


def run_training_pipeline(
    data_dir: str,
    product: str,
    config: Optional[TrainingConfig] = None,
    save_dir: Optional[str] = None
) -> Dict:
    """
    Run the full training pipeline
    
    Args:
        data_dir: Directory containing L2 data files
        product: Product ID to train on
        config: Training configuration
        save_dir: Directory to save models
    
    Returns:
        Training results
    """
    from l2_data_loader import L2DataLoader
    
    config = config or TrainingConfig()
    save_dir = save_dir or '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/models/checkpoints'
    
    print(f"=== Training Pipeline for {product} ===")
    print(f"Device: {config.device}")
    
    # Load and process data
    print("\n1. Loading data...")
    loader = L2DataLoader(data_dir)
    data = loader.load_and_process(products=[product])
    dfs = loader.to_dataframes(data)
    
    if product not in dfs:
        raise ValueError(f"No data found for {product}")
    
    df = dfs[product]
    print(f"   Loaded {len(df)} data points")
    
    # Extract features
    print("\n2. Extracting features...")
    feature_config = FeatureConfig(
        short_window=10,
        medium_window=50,
        long_window=min(100, len(df) // 5),
        prediction_horizon=config.sequence_length // 5
    )
    extractor = FeatureExtractor(feature_config)
    
    lstm_data = extractor.prepare_lstm_data(
        df,
        sequence_length=config.sequence_length,
        target_type=config.output_type,
        train_ratio=config.train_ratio,
        val_ratio=config.val_ratio
    )
    
    print(f"   Train: {lstm_data['X_train'].shape}")
    print(f"   Val: {lstm_data['X_val'].shape}")
    print(f"   Test: {lstm_data['X_test'].shape}")
    print(f"   Features: {len(lstm_data['feature_names'])}")
    
    # Create model
    print("\n3. Creating model...")
    input_size = lstm_data['X_train'].shape[-1]
    model_config = {
        'hidden_size': config.hidden_size,
        'num_layers': config.num_layers,
        'dropout': config.dropout,
        'bidirectional': config.bidirectional,
        'use_attention': config.use_attention,
        'output_type': config.output_type
    }
    model = create_model(config.model_type, input_size, model_config)
    
    num_params = sum(p.numel() for p in model.parameters())
    print(f"   Model: {config.model_type}")
    print(f"   Parameters: {num_params:,}")
    
    # Train
    print("\n4. Training...")
    trainer = Trainer(model, config, save_dir)
    train_results = trainer.train(lstm_data, verbose=True)
    
    print(f"\n   Best epoch: {train_results['best_epoch']}")
    print(f"   Best val loss: {train_results['best_val_loss']:.4f}")
    for k, v in train_results['best_metrics'].items():
        print(f"   {k}: {v:.4f}")
    
    # Evaluate on test set
    print("\n5. Evaluating on test set...")
    test_results = trainer.evaluate(lstm_data)
    print(f"   Test loss: {test_results['test_loss']:.4f}")
    for k, v in test_results['test_metrics'].items():
        print(f"   {k}: {v:.4f}")
    
    # Save metadata
    metadata = {
        'product': product,
        'config': asdict(config),
        'feature_config': asdict(feature_config),
        'feature_names': lstm_data['feature_names'],
        'feature_means': lstm_data['feature_means'],
        'feature_stds': lstm_data['feature_stds'],
        'train_results': {
            'best_epoch': train_results['best_epoch'],
            'best_val_loss': train_results['best_val_loss'],
            'best_metrics': train_results['best_metrics'],
            'total_epochs': train_results['total_epochs']
        },
        'test_results': test_results,
        'data_shape': {
            'train': list(lstm_data['X_train'].shape),
            'val': list(lstm_data['X_val'].shape),
            'test': list(lstm_data['X_test'].shape)
        },
        'timestamp': datetime.now().isoformat()
    }
    
    metadata_path = os.path.join(save_dir, 'training_metadata.json')
    with open(metadata_path, 'w') as f:
        json.dump(metadata, f, indent=2)
    
    print(f"\n6. Saved model and metadata to {save_dir}")
    
    return {
        'trainer': trainer,
        'lstm_data': lstm_data,
        'train_results': train_results,
        'test_results': test_results,
        'metadata': metadata
    }


if __name__ == "__main__":
    from l2_data_loader import discover_products
    
    data_dir = "/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/data"
    
    # Find best product
    print("Discovering products...")
    products = discover_products(data_dir)
    
    if not products:
        print("No products found!")
        exit(1)
    
    # Select product with most data
    product = list(products.keys())[0]
    print(f"\nUsing product: {product} ({products[product]} updates)")
    
    # Configure training
    config = TrainingConfig(
        model_type='lstm',
        hidden_size=64,
        num_layers=2,
        dropout=0.2,
        batch_size=32,
        learning_rate=1e-3,
        num_epochs=50,
        early_stopping_patience=10,
        sequence_length=30,
        output_type='regression'
    )
    
    # Run training
    try:
        results = run_training_pipeline(data_dir, product, config)
        print("\n=== Training Complete ===")
    except Exception as e:
        print(f"\nError during training: {e}")
        import traceback
        traceback.print_exc()
