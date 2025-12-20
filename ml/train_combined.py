#!/usr/bin/env python3
"""
Combined Multi-Product LSTM Training
Uses data from multiple products to increase training size
"""

import os
import sys
import json
from datetime import datetime
from typing import Dict, List
from collections import Counter

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
from sklearn.preprocessing import StandardScaler

sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml')
from l2_data_loader import L2DataLoader, discover_products


class SimpleLSTM(nn.Module):
    """Simple LSTM for direction prediction"""
    
    def __init__(self, input_size: int, hidden_size: int = 64, num_layers: int = 2, dropout: float = 0.3):
        super().__init__()
        
        self.lstm = nn.LSTM(
            input_size=input_size,
            hidden_size=hidden_size,
            num_layers=num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0,
            bidirectional=True
        )
        
        self.fc = nn.Sequential(
            nn.Linear(hidden_size * 2, hidden_size),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(hidden_size, 3)
        )
    
    def forward(self, x):
        lstm_out, _ = self.lstm(x)
        out = lstm_out[:, -1, :]
        return self.fc(out)


def extract_features(df: pd.DataFrame) -> pd.DataFrame:
    """Extract features from order book data"""
    features = pd.DataFrame(index=df.index)
    
    # Returns
    for h in [1, 2, 5, 10, 20]:
        ret = df['mid_price'].pct_change(h)
        features[f'return_{h}'] = ret
        features[f'return_{h}_sign'] = np.sign(ret)
    
    # Spread
    features['spread_bps'] = df['spread'] / df['mid_price'] * 10000
    features['spread_zscore'] = (features['spread_bps'] - features['spread_bps'].rolling(20).mean()) / features['spread_bps'].rolling(20).std()
    
    # Imbalance
    features['imbalance'] = df['top_imbalance']
    features['imbalance_ma5'] = df['top_imbalance'].rolling(5).mean()
    features['imbalance_ma10'] = df['top_imbalance'].rolling(10).mean()
    features['imbalance_momentum'] = features['imbalance'] - features['imbalance_ma10']
    
    # Microprice deviation
    if 'microprice' in df.columns:
        features['microprice_dev'] = (df['microprice'] - df['mid_price']) / df['mid_price'] * 10000
        features['microprice_dev_ma5'] = features['microprice_dev'].rolling(5).mean()
    
    # Volume features
    if 'best_bid_qty' in df.columns:
        total_qty = df['best_bid_qty'] + df['best_ask_qty']
        features['bid_ratio'] = df['best_bid_qty'] / total_qty
        features['qty_change'] = total_qty.pct_change()
    
    # Volatility
    features['volatility_10'] = features['return_1'].rolling(10).std()
    features['volatility_20'] = features['return_1'].rolling(20).std()
    features['vol_ratio'] = features['volatility_10'] / features['volatility_20']
    
    # Technical
    features['price_ma5_ratio'] = df['mid_price'] / df['mid_price'].rolling(5).mean() - 1
    features['price_ma20_ratio'] = df['mid_price'] / df['mid_price'].rolling(20).mean() - 1
    
    return features


def create_target(df: pd.DataFrame, horizon: int = 5, threshold_bps: float = 2.0) -> pd.Series:
    """Create 3-class target"""
    future_return = df['mid_price'].pct_change(horizon).shift(-horizon) * 10000
    
    target = pd.Series(1, index=df.index)
    target[future_return > threshold_bps] = 2
    target[future_return < -threshold_bps] = 0
    
    return target


class TradingDataset(Dataset):
    def __init__(self, X: np.ndarray, y: np.ndarray):
        self.X = torch.FloatTensor(X)
        self.y = torch.LongTensor(y)
    
    def __len__(self):
        return len(self.X)
    
    def __getitem__(self, idx):
        return self.X[idx], self.y[idx]


def train_combined_model(
    data_dir: str,
    output_dir: str,
    min_updates: int = 400,
    max_products: int = 10,
    hidden_size: int = 64,
    num_layers: int = 2,
    num_epochs: int = 100,
    batch_size: int = 64,
    learning_rate: float = 5e-4,
    sequence_length: int = 20,
    prediction_horizon: int = 5,
    threshold_bps: float = 3.0
):
    """Train on combined data from multiple products"""
    
    os.makedirs(output_dir, exist_ok=True)
    device = torch.device('mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Device: {device}")
    
    # Discover products
    print("\n1. Discovering products...")
    products = discover_products(data_dir)
    selected = [p for p, c in products.items() if c >= min_updates][:max_products]
    print(f"Selected {len(selected)} products with >= {min_updates} updates")
    
    # Load and process each product
    print("\n2. Loading and processing data...")
    loader = L2DataLoader(data_dir)
    
    all_X = []
    all_y = []
    product_info = {}
    
    for product in selected:
        print(f"  Processing {product}...")
        
        data = loader.load_and_process(products=[product])
        dfs = loader.to_dataframes(data)
        
        if product not in dfs:
            continue
        
        df = dfs[product]
        
        # Extract features
        features = extract_features(df)
        target = create_target(df, horizon=prediction_horizon, threshold_bps=threshold_bps)
        
        features['target'] = target
        features = features.dropna()
        
        if len(features) < sequence_length + 50:
            print(f"    Skipping {product}: not enough data")
            continue
        
        feature_cols = [c for c in features.columns if c != 'target']
        
        # Normalize per-product
        scaler = StandardScaler()
        features[feature_cols] = scaler.fit_transform(features[feature_cols])
        features[feature_cols] = features[feature_cols].clip(-5, 5)
        
        # Create sequences
        X, y = [], []
        values = features[feature_cols].values
        targets = features['target'].values
        
        for i in range(len(features) - sequence_length):
            X.append(values[i:i+sequence_length])
            y.append(targets[i+sequence_length-1])
        
        X = np.array(X)
        y = np.array(y)
        
        all_X.append(X)
        all_y.append(y)
        
        product_info[product] = {
            'sequences': len(X),
            'class_dist': Counter(y.tolist())
        }
        print(f"    {len(X)} sequences, classes: {Counter(y.tolist())}")
    
    if not all_X:
        print("No valid data!")
        return None
    
    # Combine all data
    X_all = np.concatenate(all_X, axis=0)
    y_all = np.concatenate(all_y, axis=0)
    
    print(f"\n3. Combined dataset: {X_all.shape}")
    print(f"Class distribution: {Counter(y_all.tolist())}")
    
    # Shuffle and split
    indices = np.random.permutation(len(X_all))
    X_all = X_all[indices]
    y_all = y_all[indices]
    
    n = len(X_all)
    train_end = int(n * 0.7)
    val_end = int(n * 0.85)
    
    X_train, y_train = X_all[:train_end], y_all[:train_end]
    X_val, y_val = X_all[train_end:val_end], y_all[train_end:val_end]
    X_test, y_test = X_all[val_end:], y_all[val_end:]
    
    print(f"Train: {len(X_train)}, Val: {len(X_val)}, Test: {len(X_test)}")
    
    # Create data loaders
    train_dataset = TradingDataset(X_train, y_train)
    val_dataset = TradingDataset(X_val, y_val)
    
    train_loader = DataLoader(train_dataset, batch_size=batch_size, shuffle=True)
    val_loader = DataLoader(val_dataset, batch_size=batch_size, shuffle=False)
    
    # Create model
    print("\n4. Creating model...")
    input_size = X_all.shape[-1]
    model = SimpleLSTM(input_size, hidden_size=hidden_size, num_layers=num_layers, dropout=0.3)
    model = model.to(device)
    print(f"Input size: {input_size}, Parameters: {sum(p.numel() for p in model.parameters()):,}")
    
    # Class weights
    class_counts = Counter(y_train.tolist())
    weights = [len(y_train) / (3 * class_counts[i]) for i in range(3)]
    class_weights = torch.FloatTensor(weights).to(device)
    
    criterion = nn.CrossEntropyLoss(weight=class_weights)
    optimizer = torch.optim.AdamW(model.parameters(), lr=learning_rate, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=num_epochs)
    
    # Training
    print("\n5. Training...")
    best_val_acc = 0
    best_epoch = 0
    patience = 15
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
        scheduler.step()
        
        # Validate
        model.eval()
        val_loss = 0
        correct = 0
        total = 0
        all_preds = []
        all_labels = []
        
        with torch.no_grad():
            for X_batch, y_batch in val_loader:
                X_batch, y_batch = X_batch.to(device), y_batch.to(device)
                output = model(X_batch)
                loss = criterion(output, y_batch)
                val_loss += loss.item()
                
                _, predicted = torch.max(output, 1)
                total += y_batch.size(0)
                correct += (predicted == y_batch).sum().item()
                
                all_preds.extend(predicted.cpu().numpy())
                all_labels.extend(y_batch.cpu().numpy())
        
        val_loss /= len(val_loader)
        val_acc = correct / total
        
        train_losses.append(train_loss)
        val_losses.append(val_loss)
        val_accs.append(val_acc)
        
        # Check for best
        if val_acc > best_val_acc:
            best_val_acc = val_acc
            best_epoch = epoch
            patience_counter = 0
            torch.save(model.state_dict(), os.path.join(output_dir, 'best_model.pt'))
        else:
            patience_counter += 1
        
        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"Epoch {epoch+1}: train={train_loss:.4f}, val={val_loss:.4f}, acc={val_acc:.4f}, best={best_val_acc:.4f}")
        
        if patience_counter >= patience:
            print(f"Early stopping at epoch {epoch+1}")
            break
    
    # Load best model
    model.load_state_dict(torch.load(os.path.join(output_dir, 'best_model.pt')))
    
    # Test evaluation
    print("\n6. Evaluating on test set...")
    model.eval()
    
    X_test_tensor = torch.FloatTensor(X_test).to(device)
    with torch.no_grad():
        test_output = model(X_test_tensor)
        test_probs = torch.softmax(test_output, dim=1).cpu().numpy()
        _, test_preds = torch.max(test_output, 1)
        test_preds = test_preds.cpu().numpy()
    
    test_acc = (test_preds == y_test).mean()
    print(f"Test accuracy: {test_acc:.4f} (random: {1/3:.4f})")
    
    for c in range(3):
        mask = y_test == c
        if mask.sum() > 0:
            acc = (test_preds[mask] == c).mean()
            print(f"  Class {c}: {acc:.4f} ({mask.sum()} samples)")
    
    # Simulated trading
    print("\n7. Simulated trading on test set...")
    
    capital = 100000.0
    position = 0
    position_size = 10000.0
    trades = 0
    equity_curve = [capital]
    
    # Generate fake prices for simulation (since we don't have aligned prices)
    # We'll simulate based on predictions
    for i in range(len(test_preds)):
        pred = test_preds[i]
        actual = y_test[i]
        conf = test_probs[i].max()
        
        # Trade based on prediction
        if conf > 0.4 and pred != 1:  # Don't trade neutral
            # Simulate P&L based on whether prediction was correct
            if pred == actual:
                if pred == 2:  # Predicted up, was up
                    pnl = position_size * 0.0003  # 3 bps gain
                else:  # Predicted down, was down  
                    pnl = position_size * 0.0003
            else:
                # Wrong prediction
                pnl = -position_size * 0.0002  # 2 bps loss
            
            capital += pnl
            trades += 1
        
        equity_curve.append(capital)
    
    final_equity = capital
    total_return = (final_equity / 100000 - 1) * 100
    
    print(f"\nSimulated Results:")
    print(f"  Return: {total_return:.2f}%")
    print(f"  Trades: {trades}")
    print(f"  Final: ${final_equity:,.2f}")
    
    # Plot
    print("\n8. Generating plots...")
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # Training curves
    ax1 = axes[0, 0]
    ax1.plot(train_losses, label='Train', alpha=0.8)
    ax1.plot(val_losses, label='Val', alpha=0.8)
    ax1.axvline(best_epoch, color='green', linestyle='--', alpha=0.5)
    ax1.set_xlabel('Epoch')
    ax1.set_ylabel('Loss')
    ax1.set_title('Training Loss')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    # Accuracy
    ax2 = axes[0, 1]
    ax2.plot(val_accs, label='Val Accuracy')
    ax2.axhline(1/3, color='gray', linestyle='--', label='Random')
    ax2.axvline(best_epoch, color='green', linestyle='--', alpha=0.5)
    ax2.set_xlabel('Epoch')
    ax2.set_ylabel('Accuracy')
    ax2.set_title(f'Validation Accuracy (Best: {best_val_acc:.4f})')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    # Confusion matrix (simplified)
    ax3 = axes[1, 0]
    from sklearn.metrics import confusion_matrix
    cm = confusion_matrix(y_test, test_preds, labels=[0, 1, 2])
    im = ax3.imshow(cm, cmap='Blues')
    ax3.set_xticks([0, 1, 2])
    ax3.set_yticks([0, 1, 2])
    ax3.set_xticklabels(['Down', 'Neutral', 'Up'])
    ax3.set_yticklabels(['Down', 'Neutral', 'Up'])
    ax3.set_xlabel('Predicted')
    ax3.set_ylabel('Actual')
    ax3.set_title('Confusion Matrix')
    for i in range(3):
        for j in range(3):
            ax3.text(j, i, cm[i, j], ha='center', va='center')
    
    # Simulated equity
    ax4 = axes[1, 1]
    ax4.plot(equity_curve)
    ax4.axhline(100000, color='gray', linestyle='--')
    ax4.set_xlabel('Trade')
    ax4.set_ylabel('Equity ($)')
    ax4.set_title(f'Simulated Equity (Return: {total_return:.2f}%)')
    ax4.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'combined_results.png'), dpi=150)
    plt.close()
    
    # Save metadata
    metadata = {
        'products': list(product_info.keys()),
        'product_info': {k: {'sequences': v['sequences'], 'classes': dict(v['class_dist'])} for k, v in product_info.items()},
        'total_sequences': len(X_all),
        'best_epoch': best_epoch,
        'best_val_acc': best_val_acc,
        'test_acc': float(test_acc),
        'simulated_return': total_return,
        'config': {
            'hidden_size': hidden_size,
            'num_layers': num_layers,
            'sequence_length': sequence_length,
            'prediction_horizon': prediction_horizon,
            'threshold_bps': threshold_bps
        },
        'timestamp': datetime.now().isoformat()
    }
    
    with open(os.path.join(output_dir, 'metadata.json'), 'w') as f:
        json.dump(metadata, f, indent=2)
    
    print(f"\nResults saved to {output_dir}")
    
    return {
        'model': model,
        'test_acc': test_acc,
        'simulated_return': total_return,
        'metadata': metadata
    }


if __name__ == "__main__":
    results = train_combined_model(
        data_dir="/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/data",
        output_dir="/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/output_combined",
        min_updates=300,
        max_products=15,
        hidden_size=64,
        num_layers=2,
        num_epochs=100,
        batch_size=64,
        learning_rate=5e-4,
        sequence_length=15,
        prediction_horizon=5,
        threshold_bps=2.0
    )
    
    if results:
        print("\n" + "="*60)
        print("TRAINING COMPLETE")
        print("="*60)
        print(f"Test Accuracy: {results['test_acc']:.4f}")
        print(f"Simulated Return: {results['simulated_return']:.2f}%")
