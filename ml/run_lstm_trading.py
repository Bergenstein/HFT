#!/usr/bin/env python3
"""
LSTM Trading System - Main Runner
Complete pipeline for training and backtesting LSTM trading models using L2 data
"""

import os
import sys
import json
import argparse
from datetime import datetime
from typing import Dict, Optional

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
from training_pipeline import TrainingConfig, run_training_pipeline, Trainer
from signal_generator import (
    TradingSignalGenerator, 
    SignalConfig, 
    LSTMBacktester,
    load_trained_model
)


def discover_and_select_product(data_dir: str, min_updates: int = 500) -> Optional[str]:
    """Discover products and select the best one for training"""
    print("\n" + "="*60)
    print("STEP 1: Discovering Products in Data")
    print("="*60)
    
    products = discover_products(data_dir)
    
    if not products:
        print("ERROR: No products found in data directory!")
        return None
    
    print(f"\nFound {len(products)} products:")
    for i, (product, count) in enumerate(list(products.items())[:15]):
        marker = " <-- Best candidate" if i == 0 and count >= min_updates else ""
        print(f"  {i+1}. {product}: {count} updates{marker}")
    
    # Select product with most updates that meets minimum threshold
    for product, count in products.items():
        if count >= min_updates:
            print(f"\nSelected: {product} ({count} updates)")
            return product
    
    print(f"\nWARNING: No product has >= {min_updates} updates")
    product = list(products.keys())[0]
    print(f"Using {product} anyway ({products[product]} updates)")
    return product


def train_model(
    data_dir: str,
    product: str,
    output_dir: str,
    config: Optional[TrainingConfig] = None
) -> Dict:
    """Train LSTM model on selected product"""
    print("\n" + "="*60)
    print("STEP 2: Training LSTM Model")
    print("="*60)
    
    config = config or TrainingConfig(
        model_type='lstm',
        hidden_size=64,
        num_layers=2,
        dropout=0.2,
        bidirectional=True,
        use_attention=True,
        batch_size=32,
        learning_rate=1e-3,
        num_epochs=50,
        early_stopping_patience=10,
        sequence_length=30,
        output_type='regression'
    )
    
    results = run_training_pipeline(data_dir, product, config, output_dir)
    return results


def run_backtest(
    data_dir: str,
    product: str,
    model_dir: str,
    output_dir: str
) -> Dict:
    """Run backtest on trained model"""
    print("\n" + "="*60)
    print("STEP 3: Running Backtest")
    print("="*60)
    
    # Load model
    checkpoint_path = os.path.join(model_dir, 'best_model.pt')
    metadata_path = os.path.join(model_dir, 'training_metadata.json')
    
    if not os.path.exists(checkpoint_path):
        raise FileNotFoundError(f"Model checkpoint not found: {checkpoint_path}")
    
    print(f"\nLoading model from {checkpoint_path}")
    model, metadata = load_trained_model(checkpoint_path, metadata_path)
    
    # Create feature extractor with same config
    feature_config = FeatureConfig(**metadata.get('feature_config', {}))
    feature_extractor = FeatureExtractor(feature_config)
    
    # Create signal generator
    signal_config = SignalConfig(
        strong_buy_threshold=10.0,
        buy_threshold=5.0,
        sell_threshold=-5.0,
        strong_sell_threshold=-10.0,
        min_confidence=0.5,
        use_smoothing=True,
        smoothing_window=3
    )
    
    signal_generator = TradingSignalGenerator(
        model=model,
        feature_extractor=feature_extractor,
        config=signal_config
    )
    
    # Set normalization parameters
    signal_generator.set_normalization_params(
        metadata['feature_means'],
        metadata['feature_stds']
    )
    
    # Load test data
    print(f"\nLoading data for {product}...")
    loader = L2DataLoader(data_dir)
    data = loader.load_and_process(products=[product])
    dfs = loader.to_dataframes(data)
    
    if product not in dfs:
        raise ValueError(f"No data for {product}")
    
    df = dfs[product]
    print(f"Loaded {len(df)} data points")
    
    # Use last 30% for backtest (test set)
    test_start_idx = int(len(df) * 0.7)
    test_df = df.iloc[test_start_idx:]
    print(f"Using {len(test_df)} points for backtest")
    
    # Create backtester
    backtester = LSTMBacktester(
        signal_generator=signal_generator,
        initial_capital=100000.0,
        transaction_cost_bps=1.0,
        max_position_value=50000.0
    )
    
    # Run backtest
    print("\nRunning backtest...")
    sequence_length = metadata['config']['sequence_length']
    results = backtester.run_backtest(
        test_df,
        sequence_length=sequence_length,
        min_interval=5
    )
    
    # Print results
    print("\n" + "-"*40)
    print("BACKTEST RESULTS")
    print("-"*40)
    print(f"Initial Capital:    ${results['initial_capital']:,.2f}")
    print(f"Final Equity:       ${results['final_equity']:,.2f}")
    print(f"Total Return:       {results['total_return_pct']:.2f}%")
    print(f"Sharpe Ratio:       {results['sharpe_ratio']:.2f}")
    print(f"Max Drawdown:       {results['max_drawdown_pct']:.2f}%")
    print(f"Number of Trades:   {results['num_trades']}")
    print(f"Win Rate:           {results['win_rate_pct']:.1f}%")
    
    # Save results
    results_summary = {k: v for k, v in results.items() if k != 'equity_curve'}
    results_path = os.path.join(output_dir, 'backtest_results.json')
    with open(results_path, 'w') as f:
        json.dump(results_summary, f, indent=2, default=str)
    
    # Plot equity curve
    if 'equity_curve' in results and len(results['equity_curve']) > 0:
        plot_results(results, output_dir, product)
    
    return results


def plot_results(results: Dict, output_dir: str, product: str):
    """Plot backtest results"""
    print("\nGenerating plots...")
    
    equity_df = results['equity_curve']
    
    fig, axes = plt.subplots(3, 1, figsize=(14, 10))
    
    # Equity curve
    ax1 = axes[0]
    ax1.plot(equity_df.index, equity_df['equity'], 'b-', linewidth=1)
    ax1.axhline(y=results['initial_capital'], color='gray', linestyle='--', alpha=0.5)
    ax1.set_title(f'Equity Curve - {product}')
    ax1.set_ylabel('Equity ($)')
    ax1.grid(True, alpha=0.3)
    
    # Position
    ax2 = axes[1]
    ax2.fill_between(equity_df.index, 0, equity_df['position'], 
                     where=equity_df['position'] > 0, color='green', alpha=0.3, label='Long')
    ax2.fill_between(equity_df.index, 0, equity_df['position'],
                     where=equity_df['position'] < 0, color='red', alpha=0.3, label='Short')
    ax2.axhline(y=0, color='black', linewidth=0.5)
    ax2.set_title('Position')
    ax2.set_ylabel('Position Size')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    # Predictions
    ax3 = axes[2]
    ax3.plot(equity_df.index, equity_df['prediction'], 'purple', linewidth=0.5, alpha=0.7)
    ax3.axhline(y=0, color='black', linewidth=0.5)
    ax3.axhline(y=5, color='green', linestyle='--', alpha=0.3)
    ax3.axhline(y=-5, color='red', linestyle='--', alpha=0.3)
    ax3.set_title('Model Predictions (bps)')
    ax3.set_ylabel('Predicted Return (bps)')
    ax3.set_xlabel('Time')
    ax3.grid(True, alpha=0.3)
    
    plt.tight_layout()
    
    plot_path = os.path.join(output_dir, 'backtest_results.png')
    plt.savefig(plot_path, dpi=150, bbox_inches='tight')
    plt.close()
    
    print(f"Saved plot to {plot_path}")


def run_full_pipeline(
    data_dir: str,
    output_dir: str,
    product: Optional[str] = None,
    skip_training: bool = False
):
    """Run the complete LSTM trading pipeline"""
    
    print("\n" + "="*60)
    print("LSTM TRADING SYSTEM")
    print("="*60)
    print(f"Data directory: {data_dir}")
    print(f"Output directory: {output_dir}")
    print(f"Timestamp: {datetime.now().isoformat()}")
    
    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    
    # Step 1: Discover products
    if product is None:
        product = discover_and_select_product(data_dir, min_updates=300)
        if product is None:
            print("\nFailed to find suitable product. Exiting.")
            return
    else:
        print(f"\nUsing specified product: {product}")
    
    model_dir = os.path.join(output_dir, 'checkpoints')
    
    # Step 2: Train model
    if not skip_training:
        try:
            train_results = train_model(data_dir, product, model_dir)
        except Exception as e:
            print(f"\nTraining failed: {e}")
            import traceback
            traceback.print_exc()
            return
    else:
        print("\nSkipping training (using existing model)")
    
    # Step 3: Run backtest
    try:
        backtest_results = run_backtest(data_dir, product, model_dir, output_dir)
    except Exception as e:
        print(f"\nBacktest failed: {e}")
        import traceback
        traceback.print_exc()
        return
    
    # Summary
    print("\n" + "="*60)
    print("PIPELINE COMPLETE")
    print("="*60)
    print(f"Product: {product}")
    print(f"Model saved to: {model_dir}")
    print(f"Results saved to: {output_dir}")
    
    return {
        'product': product,
        'backtest_results': {k: v for k, v in backtest_results.items() if k != 'equity_curve'}
    }


def main():
    parser = argparse.ArgumentParser(description='LSTM Trading System')
    parser.add_argument('--data-dir', type=str, 
                        default='/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/data',
                        help='Directory containing L2 data files')
    parser.add_argument('--output-dir', type=str,
                        default='/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/output',
                        help='Directory for output files')
    parser.add_argument('--product', type=str, default=None,
                        help='Specific product to use (auto-select if not specified)')
    parser.add_argument('--skip-training', action='store_true',
                        help='Skip training and use existing model')
    parser.add_argument('--discover-only', action='store_true',
                        help='Only discover products, do not train')
    
    args = parser.parse_args()
    
    if args.discover_only:
        discover_and_select_product(args.data_dir)
        return
    
    run_full_pipeline(
        data_dir=args.data_dir,
        output_dir=args.output_dir,
        product=args.product,
        skip_training=args.skip_training
    )


if __name__ == "__main__":
    main()
