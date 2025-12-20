#!/usr/bin/env python3
"""
Simple LSTM Trading System
Uses classification instead of regression for more robust signals
"""

import os
import sys
import json
from datetime import datetime
from typing import Dict, Optional, List

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
from tqdm import tqdm

# Add module paths
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/models')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/features')

from l2_data_loader import L2DataLoader, discover_products


class SimpleLSTM(nn.Module):
    """Simple LSTM for direction prediction (binary classification)"""
    
    def __init__(self, input_size: int, hidden_size: int = 32, num_layers: int = 1, dropout: float = 0.2):
        super().__init__()
        
        self.lstm = nn.LSTM(
            input_size=input_size,
            hidden_size=hidden_size,
            num_layers=num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0
        )
        
        self.fc = nn.Sequential(
            nn.Linear(hidden_size, hidden_size // 2),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(hidden_size // 2, 3)  # 3 classes: down, neutral, up
        )
    
    def forward(self, x):
        lstm_out, _ = self.lstm(x)
        # Use last time step
        out = lstm_out[:, -1, :]
        return self.fc(out)


def extract_simple_features(df: pd.DataFrame) -> pd.DataFrame:
    """Extract simple, robust features from order book data"""
    features = pd.DataFrame(index=df.index)
    
    # Price returns at different horizons
    for h in [1, 2, 5, 10]:
        features[f'return_{h}'] = df['mid_price'].pct_change(h)
    
    # Log returns
    features['log_return'] = np.log(df['mid_price']).diff()
    
    # Spread
    features['spread_bps'] = df['spread'] / df['mid_price'] * 10000
    features['spread_change'] = features['spread_bps'].diff()
    
    # Order book imbalance
    features['imbalance'] = df['top_imbalance']
    features['imbalance_ma5'] = df['top_imbalance'].rolling(5).mean()
    features['imbalance_change'] = df['top_imbalance'].diff()
    
    # Microprice deviation
    if 'microprice' in df.columns:
        features['microprice_dev'] = (df['microprice'] - df['mid_price']) / df['mid_price'] * 10000
    
    # Volume ratios
    if 'best_bid_qty' in df.columns and 'best_ask_qty' in df.columns:
        features['qty_ratio'] = df['best_bid_qty'] / (df['best_bid_qty'] + df['best_ask_qty'])
        features['qty_ratio_change'] = features['qty_ratio'].diff()
    
    # Volatility (rolling std of returns)
    features['volatility_5'] = features['log_return'].rolling(5).std()
    features['volatility_10'] = features['log_return'].rolling(10).std()
    
    # Momentum indicators
    features['momentum_5'] = df['mid_price'].pct_change(5)
    features['ma_ratio_5_20'] = df['mid_price'].rolling(5).mean() / df['mid_price'].rolling(20).mean() - 1
    
    return features


def create_classification_target(df: pd.DataFrame, horizon: int = 5, threshold_bps: float = 2.0) -> pd.Series:
    """
    Create 3-class target: 0=down, 1=neutral, 2=up
    """
    future_return = df['mid_price'].pct_change(horizon).shift(-horizon) * 10000  # in bps
    
    target = pd.Series(1, index=df.index)  # Default neutral
    target[future_return > threshold_bps] = 2  # Up
    target[future_return < -threshold_bps] = 0  # Down
    
    return target


class TradingDataset(Dataset):
    def __init__(self, X: np.ndarray, y: np.ndarray):
        self.X = torch.FloatTensor(X)
        self.y = torch.LongTensor(y)
    
    def __len__(self):
        return len(self.X)
    
    def __getitem__(self, idx):
        return self.X[idx], self.y[idx]


def train_simple_model(
    data_dir: str,
    output_dir: str,
    hidden_size: int = 32,
    num_epochs: int = 50,
    batch_size: int = 32,
    learning_rate: float = 1e-3,
    sequence_length: int = 20,
    prediction_horizon: int = 5,
    threshold_bps: float = 3.0
):
    """Train simple LSTM classifier"""
    
    os.makedirs(output_dir, exist_ok=True)
    device = torch.device('mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Device: {device}")
    
    # Load data
    print("\n1. Loading data...")
    loader = L2DataLoader(data_dir)
    products = discover_products(data_dir)
    product = list(products.keys())[0]
    print(f"Using product: {product} ({products[product]} updates)")
    
    data = loader.load_and_process(products=[product])
    dfs = loader.to_dataframes(data)
    df = dfs[product]
    print(f"Data shape: {df.shape}")
    
    # Extract features
    print("\n2. Extracting features...")
    features = extract_simple_features(df)
    target = create_classification_target(df, horizon=prediction_horizon, threshold_bps=threshold_bps)
    
    # Combine and clean
    features['target'] = target
    features = features.dropna()
    
    feature_cols = [c for c in features.columns if c != 'target']
    print(f"Features: {len(feature_cols)}")
    print(f"Valid rows: {len(features)}")
    
    # Check class distribution
    class_counts = features['target'].value_counts().sort_index()
    print(f"Class distribution: Down={class_counts.get(0, 0)}, Neutral={class_counts.get(1, 0)}, Up={class_counts.get(2, 0)}")
    
    # Normalize features
    feature_means = features[feature_cols].mean()
    feature_stds = features[feature_cols].std().replace(0, 1)
    features[feature_cols] = (features[feature_cols] - feature_means) / feature_stds
    features[feature_cols] = features[feature_cols].clip(-5, 5)
    
    # Create sequences
    print("\n3. Creating sequences...")
    X, y = [], []
    values = features[feature_cols].values
    targets = features['target'].values
    
    for i in range(len(features) - sequence_length):
        X.append(values[i:i+sequence_length])
        y.append(targets[i+sequence_length-1])
    
    X = np.array(X)
    y = np.array(y)
    print(f"Sequences: {X.shape}")
    
    # Train/val/test split
    n = len(X)
    train_end = int(n * 0.7)
    val_end = int(n * 0.85)
    
    X_train, y_train = X[:train_end], y[:train_end]
    X_val, y_val = X[train_end:val_end], y[train_end:val_end]
    X_test, y_test = X[val_end:], y[val_end:]
    
    print(f"Train: {len(X_train)}, Val: {len(X_val)}, Test: {len(X_test)}")
    
    # Create data loaders
    train_dataset = TradingDataset(X_train, y_train)
    val_dataset = TradingDataset(X_val, y_val)
    
    train_loader = DataLoader(train_dataset, batch_size=batch_size, shuffle=True)
    val_loader = DataLoader(val_dataset, batch_size=batch_size, shuffle=False)
    
    # Create model
    print("\n4. Creating model...")
    input_size = X.shape[-1]
    model = SimpleLSTM(input_size, hidden_size=hidden_size, num_layers=1, dropout=0.2)
    model = model.to(device)
    print(f"Parameters: {sum(p.numel() for p in model.parameters()):,}")
    
    # Class weights for imbalanced data
    class_weights = torch.FloatTensor([
        len(y_train) / (3 * (y_train == 0).sum()),
        len(y_train) / (3 * (y_train == 1).sum()),
        len(y_train) / (3 * (y_train == 2).sum())
    ]).to(device)
    
    criterion = nn.CrossEntropyLoss(weight=class_weights)
    optimizer = torch.optim.Adam(model.parameters(), lr=learning_rate, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.ReduceLROnPlateau(optimizer, patience=5, factor=0.5)
    
    # Training
    print("\n5. Training...")
    best_val_acc = 0
    best_epoch = 0
    patience = 10
    patience_counter = 0
    
    train_losses, val_losses, val_accs = [], [], []
    
    for epoch in range(num_epochs):
        # Train
        model.train()
        train_loss = 0
        for X_batch, y_batch in train_loader:
            X_batch, y_batch = X_batch.to(device), y_batch.to(device)
            
            optimizer.zero_grad()
            output = model(X_batch)
            loss = criterion(output, y_batch)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            
            train_loss += loss.item()
        
        train_loss /= len(train_loader)
        
        # Validate
        model.eval()
        val_loss = 0
        correct = 0
        total = 0
        
        with torch.no_grad():
            for X_batch, y_batch in val_loader:
                X_batch, y_batch = X_batch.to(device), y_batch.to(device)
                output = model(X_batch)
                loss = criterion(output, y_batch)
                val_loss += loss.item()
                
                _, predicted = torch.max(output, 1)
                total += y_batch.size(0)
                correct += (predicted == y_batch).sum().item()
        
        val_loss /= len(val_loader)
        val_acc = correct / total
        
        scheduler.step(val_loss)
        
        train_losses.append(train_loss)
        val_losses.append(val_loss)
        val_accs.append(val_acc)
        
        # Early stopping
        if val_acc > best_val_acc:
            best_val_acc = val_acc
            best_epoch = epoch
            patience_counter = 0
            torch.save(model.state_dict(), os.path.join(output_dir, 'best_model.pt'))
        else:
            patience_counter += 1
        
        if (epoch + 1) % 5 == 0 or epoch == 0:
            print(f"Epoch {epoch+1}: train_loss={train_loss:.4f}, val_loss={val_loss:.4f}, val_acc={val_acc:.4f}, best={best_val_acc:.4f}")
        
        if patience_counter >= patience:
            print(f"Early stopping at epoch {epoch+1}")
            break
    
    # Load best model
    model.load_state_dict(torch.load(os.path.join(output_dir, 'best_model.pt')))
    
    # Test evaluation
    print(f"\n6. Evaluating on test set...")
    model.eval()
    
    X_test_tensor = torch.FloatTensor(X_test).to(device)
    with torch.no_grad():
        test_output = model(X_test_tensor)
        _, test_preds = torch.max(test_output, 1)
        test_preds = test_preds.cpu().numpy()
        test_probs = torch.softmax(test_output, dim=1).cpu().numpy()
    
    test_acc = (test_preds == y_test).mean()
    print(f"Test accuracy: {test_acc:.4f}")
    
    # Per-class accuracy
    for c in range(3):
        mask = y_test == c
        if mask.sum() > 0:
            acc = (test_preds[mask] == y_test[mask]).mean()
            print(f"  Class {c} accuracy: {acc:.4f} (n={mask.sum()})")
    
    # Backtest simulation
    print("\n7. Running backtest...")
    
    # Get prices for backtest
    prices = df['mid_price'].values[sequence_length + val_end:]
    if len(prices) > len(test_preds):
        prices = prices[:len(test_preds)]
    elif len(test_preds) > len(prices):
        test_preds = test_preds[:len(prices)]
        test_probs = test_probs[:len(prices)]
    
    # Simple backtest
    initial_capital = 100000.0
    position = 0.0  # Position in units
    capital = initial_capital
    position_value = 10000.0  # Trade $10k at a time
    
    equity_curve = [capital]
    positions = [0]
    trades = 0
    
    for i in range(len(test_preds) - 1):
        pred = test_preds[i]
        prob = test_probs[i]
        confidence = prob.max()
        
        current_price = prices[i]
        next_price = prices[i + 1]
        
        # Only trade with high confidence
        if confidence > 0.35:
            target_position = 0
            if pred == 2:  # Up prediction
                target_position = position_value / current_price
            elif pred == 0:  # Down prediction
                target_position = -position_value / current_price
            
            # Execute trade
            if target_position != position:
                # Close old position
                capital += position * current_price
                # Open new position
                position = target_position
                capital -= position * current_price
                trades += 1
        
        # Mark to market
        equity = capital + position * next_price
        equity_curve.append(equity)
        positions.append(position)
    
    # Close final position
    if position != 0:
        capital += position * prices[-1]
        trades += 1
    
    final_equity = capital
    total_return = (final_equity / initial_capital - 1) * 100
    
    # Calculate metrics
    equity_arr = np.array(equity_curve)
    returns = np.diff(equity_arr) / equity_arr[:-1]
    sharpe = np.mean(returns) / np.std(returns) * np.sqrt(252 * 24 * 60) if np.std(returns) > 0 else 0
    
    max_dd = 0
    peak = equity_arr[0]
    for eq in equity_arr:
        if eq > peak:
            peak = eq
        dd = (peak - eq) / peak
        if dd > max_dd:
            max_dd = dd
    
    print(f"\n" + "-"*40)
    print("BACKTEST RESULTS")
    print("-"*40)
    print(f"Initial Capital:    ${initial_capital:,.2f}")
    print(f"Final Equity:       ${final_equity:,.2f}")
    print(f"Total Return:       {total_return:.2f}%")
    print(f"Sharpe Ratio:       {sharpe:.2f}")
    print(f"Max Drawdown:       {max_dd*100:.2f}%")
    print(f"Number of Trades:   {trades}")
    
    # Plot results
    print("\n8. Generating plots...")
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # Training curves
    ax1 = axes[0, 0]
    ax1.plot(train_losses, label='Train Loss')
    ax1.plot(val_losses, label='Val Loss')
    ax1.axvline(best_epoch, color='green', linestyle='--', label=f'Best ({best_epoch})')
    ax1.set_xlabel('Epoch')
    ax1.set_ylabel('Loss')
    ax1.set_title('Training Curves')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    # Validation accuracy
    ax2 = axes[0, 1]
    ax2.plot(val_accs)
    ax2.axhline(1/3, color='gray', linestyle='--', label='Random')
    ax2.set_xlabel('Epoch')
    ax2.set_ylabel('Accuracy')
    ax2.set_title(f'Validation Accuracy (Best: {best_val_acc:.4f})')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    # Equity curve
    ax3 = axes[1, 0]
    ax3.plot(equity_curve)
    ax3.axhline(initial_capital, color='gray', linestyle='--')
    ax3.set_xlabel('Time')
    ax3.set_ylabel('Equity ($)')
    ax3.set_title(f'Equity Curve (Return: {total_return:.2f}%)')
    ax3.grid(True, alpha=0.3)
    
    # Prediction distribution
    ax4 = axes[1, 1]
    pred_counts = pd.Series(test_preds).value_counts().sort_index()
    ax4.bar(['Down', 'Neutral', 'Up'], [pred_counts.get(0, 0), pred_counts.get(1, 0), pred_counts.get(2, 0)])
    ax4.set_xlabel('Prediction')
    ax4.set_ylabel('Count')
    ax4.set_title('Prediction Distribution')
    ax4.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'simple_lstm_results.png'), dpi=150)
    plt.close()
    
    print(f"Saved plot to {output_dir}/simple_lstm_results.png")
    
    # Save metadata
    metadata = {
        'product': product,
        'best_epoch': best_epoch,
        'best_val_acc': best_val_acc,
        'test_acc': float(test_acc),
        'backtest': {
            'total_return': total_return,
            'sharpe': sharpe,
            'max_drawdown': max_dd * 100,
            'trades': trades
        },
        'config': {
            'hidden_size': hidden_size,
            'sequence_length': sequence_length,
            'prediction_horizon': prediction_horizon,
            'threshold_bps': threshold_bps
        },
        'feature_names': feature_cols,
        'feature_means': feature_means.to_dict(),
        'feature_stds': feature_stds.to_dict(),
        'timestamp': datetime.now().isoformat()
    }
    
    with open(os.path.join(output_dir, 'metadata.json'), 'w') as f:
        json.dump(metadata, f, indent=2)
    
    print(f"\nResults saved to {output_dir}")
    
    return {
        'model': model,
        'metadata': metadata,
        'test_acc': test_acc,
        'backtest_return': total_return
    }


if __name__ == "__main__":
    data_dir = "/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/data"
    output_dir = "/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/output_simple"
    
    results = train_simple_model(
        data_dir=data_dir,
        output_dir=output_dir,
        hidden_size=32,
        num_epochs=100,
        batch_size=32,
        learning_rate=1e-3,
        sequence_length=15,
        prediction_horizon=5,
        threshold_bps=2.0  # Lower threshold for more action
    )
    
    print("\n" + "="*60)
    print("COMPLETE")
    print("="*60)
    print(f"Test Accuracy: {results['test_acc']:.4f}")
    print(f"Backtest Return: {results['backtest_return']:.2f}%")
