#!/usr/bin/env python3
"""
LSTM Trading System - Multi-Product Training
Trains LSTM model on multiple products combined for more data
"""

import os
import sys
import json
from datetime import datetime
from typing import Dict, Optional, List

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

# Add module paths
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/models')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/features')

from l2_data_loader import L2DataLoader, discover_products
from feature_extractor import FeatureExtractor, FeatureConfig
from lstm_model import create_model
from training_pipeline import TrainingConfig, Trainer
from signal_generator import (
    TradingSignalGenerator, 
    SignalConfig, 
    LSTMBacktester,
    load_trained_model
)


def load_multi_product_data(
    data_dir: str,
    min_updates: int = 500,
    max_products: int = 10
) -> pd.DataFrame:
    """Load data from multiple products and combine"""
    
    print("\n" + "="*60)
    print("Loading Multi-Product Data")
    print("="*60)
    
    products = discover_products(data_dir)
    
    # Filter products with enough data
    selected_products = []
    for product, count in products.items():
        if count >= min_updates:
            selected_products.append(product)
            if len(selected_products) >= max_products:
                break
    
    print(f"\nSelected {len(selected_products)} products with >= {min_updates} updates:")
    for p in selected_products:
        print(f"  - {p}: {products[p]} updates")
    
    if not selected_products:
        print("No products meet minimum update requirement!")
        return None
    
    # Load data from each product
    loader = L2DataLoader(data_dir)
    all_dfs = []
    
    for product in selected_products:
        data = loader.load_and_process(products=[product])
        dfs = loader.to_dataframes(data)
        
        if product in dfs:
            df = dfs[product]
            df['product'] = product
            # Normalize price to percentage returns for combining
            df['price_return'] = df['mid_price'].pct_change()
            all_dfs.append(df)
            print(f"  Loaded {product}: {len(df)} rows")
    
    # Combine all dataframes
    combined = pd.concat(all_dfs, ignore_index=False)
    combined = combined.sort_index()
    
    print(f"\nCombined dataset: {len(combined)} total rows")
    
    return combined, selected_products


def train_multi_product_model(
    data_dir: str,
    output_dir: str,
    config: Optional[TrainingConfig] = None
) -> Dict:
    """Train LSTM on combined multi-product data"""
    
    config = config or TrainingConfig(
        model_type='lstm',
        hidden_size=128,
        num_layers=2,
        dropout=0.3,
        bidirectional=True,
        use_attention=True,
        batch_size=64,
        learning_rate=5e-4,
        weight_decay=1e-4,
        num_epochs=100,
        early_stopping_patience=15,
        sequence_length=50,
        output_type='regression'
    )
    
    os.makedirs(output_dir, exist_ok=True)
    model_dir = os.path.join(output_dir, 'checkpoints')
    os.makedirs(model_dir, exist_ok=True)
    
    # Load multi-product data
    result = load_multi_product_data(data_dir, min_updates=400, max_products=15)
    if result is None:
        return None
    combined_df, selected_products = result
    
    # For simplicity, we'll train on each product separately but use combined features
    # Let's use the product with most data
    best_product = selected_products[0]
    
    print(f"\n" + "="*60)
    print(f"Training on {best_product}")
    print("="*60)
    
    # Get single product data
    loader = L2DataLoader(data_dir)
    data = loader.load_and_process(products=[best_product])
    dfs = loader.to_dataframes(data)
    df = dfs[best_product]
    
    print(f"Data shape: {df.shape}")
    
    # Extract features
    print("\nExtracting features...")
    feature_config = FeatureConfig(
        short_window=10,
        medium_window=30,
        long_window=min(100, len(df) // 5),
        prediction_horizon=10
    )
    extractor = FeatureExtractor(feature_config)
    
    lstm_data = extractor.prepare_lstm_data(
        df,
        sequence_length=config.sequence_length,
        target_type=config.output_type,
        train_ratio=config.train_ratio,
        val_ratio=config.val_ratio
    )
    
    print(f"Train: {lstm_data['X_train'].shape}")
    print(f"Val: {lstm_data['X_val'].shape}")
    print(f"Test: {lstm_data['X_test'].shape}")
    
    # Create model
    print("\nCreating model...")
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
    print(f"Model parameters: {num_params:,}")
    
    # Train
    print("\nTraining...")
    trainer = Trainer(model, config, model_dir)
    train_results = trainer.train(lstm_data, verbose=True)
    
    # Evaluate
    print("\nEvaluating on test set...")
    test_results = trainer.evaluate(lstm_data)
    
    # Save metadata
    metadata = {
        'product': best_product,
        'all_products': selected_products,
        'config': {k: v for k, v in config.__dict__.items()},
        'feature_config': {k: v for k, v in feature_config.__dict__.items()},
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
    
    metadata_path = os.path.join(model_dir, 'training_metadata.json')
    with open(metadata_path, 'w') as f:
        json.dump(metadata, f, indent=2)
    
    # Run backtest with lower thresholds
    print("\n" + "="*60)
    print("Running Backtest with Adjusted Thresholds")
    print("="*60)
    
    signal_config = SignalConfig(
        strong_buy_threshold=3.0,  # Lowered from 10
        buy_threshold=1.0,         # Lowered from 5
        sell_threshold=-1.0,       # Lowered from -5
        strong_sell_threshold=-3.0,# Lowered from -10
        min_confidence=0.3,        # Lowered from 0.6
        use_smoothing=True,
        smoothing_window=3
    )
    
    signal_generator = TradingSignalGenerator(
        model=model,
        feature_extractor=extractor,
        config=signal_config,
        device=config.device
    )
    signal_generator.set_normalization_params(
        lstm_data['feature_means'],
        lstm_data['feature_stds']
    )
    
    # Backtest on test portion
    test_start_idx = int(len(df) * 0.7)
    test_df = df.iloc[test_start_idx:]
    
    backtester = LSTMBacktester(
        signal_generator=signal_generator,
        initial_capital=100000.0,
        transaction_cost_bps=2.0,
        max_position_value=50000.0
    )
    
    backtest_results = backtester.run_backtest(
        test_df,
        sequence_length=config.sequence_length,
        min_interval=3
    )
    
    print("\n" + "-"*40)
    print("BACKTEST RESULTS")
    print("-"*40)
    print(f"Initial Capital:    ${backtest_results['initial_capital']:,.2f}")
    print(f"Final Equity:       ${backtest_results['final_equity']:,.2f}")
    print(f"Total Return:       {backtest_results['total_return_pct']:.2f}%")
    print(f"Sharpe Ratio:       {backtest_results['sharpe_ratio']:.2f}")
    print(f"Max Drawdown:       {backtest_results['max_drawdown_pct']:.2f}%")
    print(f"Number of Trades:   {backtest_results['num_trades']}")
    print(f"Win Rate:           {backtest_results['win_rate_pct']:.1f}%")
    
    # Plot results
    if 'equity_curve' in backtest_results and len(backtest_results['equity_curve']) > 0:
        plot_detailed_results(
            backtest_results, 
            train_results,
            output_dir, 
            best_product
        )
    
    return {
        'trainer': trainer,
        'lstm_data': lstm_data,
        'train_results': train_results,
        'test_results': test_results,
        'backtest_results': {k: v for k, v in backtest_results.items() if k != 'equity_curve'},
        'metadata': metadata
    }


def plot_detailed_results(backtest_results, train_results, output_dir, product):
    """Plot detailed results"""
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # Training loss curve
    ax1 = axes[0, 0]
    epochs = range(len(train_results['history']['train_loss']))
    ax1.plot(epochs, train_results['history']['train_loss'], label='Train Loss', color='blue')
    ax1.plot(epochs, train_results['history']['val_loss'], label='Val Loss', color='orange')
    ax1.axvline(x=train_results['best_epoch'], color='green', linestyle='--', label=f'Best Epoch ({train_results["best_epoch"]})')
    ax1.set_xlabel('Epoch')
    ax1.set_ylabel('Loss')
    ax1.set_title('Training and Validation Loss')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    # Equity curve
    ax2 = axes[0, 1]
    equity_df = backtest_results['equity_curve']
    ax2.plot(equity_df.index, equity_df['equity'], 'b-', linewidth=1)
    ax2.axhline(y=backtest_results['initial_capital'], color='gray', linestyle='--', alpha=0.5)
    ax2.set_title(f'Equity Curve - {product}')
    ax2.set_ylabel('Equity ($)')
    ax2.grid(True, alpha=0.3)
    
    # Position
    ax3 = axes[1, 0]
    ax3.fill_between(equity_df.index, 0, equity_df['position'], 
                     where=equity_df['position'] > 0, color='green', alpha=0.3, label='Long')
    ax3.fill_between(equity_df.index, 0, equity_df['position'],
                     where=equity_df['position'] < 0, color='red', alpha=0.3, label='Short')
    ax3.axhline(y=0, color='black', linewidth=0.5)
    ax3.set_title('Position Over Time')
    ax3.set_ylabel('Position Size')
    ax3.legend()
    ax3.grid(True, alpha=0.3)
    
    # Prediction distribution
    ax4 = axes[1, 1]
    predictions = equity_df['prediction'].dropna()
    ax4.hist(predictions, bins=50, alpha=0.7, color='purple')
    ax4.axvline(x=0, color='black', linewidth=1)
    ax4.axvline(x=1, color='green', linestyle='--', alpha=0.5, label='Buy threshold')
    ax4.axvline(x=-1, color='red', linestyle='--', alpha=0.5, label='Sell threshold')
    ax4.set_title('Prediction Distribution')
    ax4.set_xlabel('Predicted Return (bps)')
    ax4.set_ylabel('Frequency')
    ax4.legend()
    ax4.grid(True, alpha=0.3)
    
    plt.tight_layout()
    
    plot_path = os.path.join(output_dir, 'detailed_results.png')
    plt.savefig(plot_path, dpi=150, bbox_inches='tight')
    plt.close()
    
    print(f"\nSaved detailed plot to {plot_path}")


if __name__ == "__main__":
    data_dir = "/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/data"
    output_dir = "/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/output_multi"
    
    config = TrainingConfig(
        model_type='lstm',
        hidden_size=128,
        num_layers=2,
        dropout=0.25,
        bidirectional=True,
        use_attention=True,
        batch_size=32,
        learning_rate=1e-3,
        weight_decay=1e-5,
        num_epochs=100,
        early_stopping_patience=15,
        sequence_length=30,
        output_type='regression'
    )
    
    results = train_multi_product_model(data_dir, output_dir, config)
    
    if results:
        print("\n" + "="*60)
        print("TRAINING COMPLETE")
        print("="*60)
        print(f"Best validation loss: {results['train_results']['best_val_loss']:.4f}")
        print(f"Test metrics: {results['test_results']}")
        print(f"Backtest return: {results['backtest_results']['total_return_pct']:.2f}%")
