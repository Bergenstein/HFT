# LSTM Trading System for L2 Order Book Data

A complete machine learning pipeline for training LSTM models on Coinbase Level 2 (L2) order book data to generate trading signals.

## Overview

This system processes real L2 order book data in NDJSON format and uses LSTM neural networks to predict short-term price movements. The pipeline includes:

1. **Data Loading** - Parse Coinbase L2 NDJSON websocket recordings
2. **Feature Extraction** - Extract microstructure features from order books
3. **Model Training** - Train LSTM classifiers/regressors
4. **Signal Generation** - Convert predictions to trading signals
5. **Backtesting** - Evaluate strategy performance

## Project Structure

```
ml/
├── l2_data_loader.py      # L2 NDJSON data parser and order book reconstructor
├── features/
│   └── feature_extractor.py  # Feature extraction from order book data
├── models/
│   └── lstm_model.py      # LSTM, Transformer model architectures
├── training_pipeline.py   # Full training pipeline with checkpointing
├── signal_generator.py    # Trading signal generation and backtesting
├── run_lstm_trading.py    # Main runner script
├── train_combined.py      # Multi-product combined training
├── train_simple_lstm.py   # Simplified LSTM classifier
└── output_combined/       # Trained model outputs
    ├── best_model.pt      # Best model checkpoint
    ├── metadata.json      # Training metadata
    └── combined_results.png  # Training/results visualization
```

## Features Extracted

The system extracts 25+ features from L2 order book data:

### Price Features
- Returns at multiple horizons (1, 2, 5, 10, 20 ticks)
- Log returns
- Price momentum indicators
- Moving average ratios

### Order Book Features
- Bid-ask spread (in bps)
- Spread z-score
- Top-of-book imbalance
- Imbalance moving averages
- Imbalance momentum
- Microprice deviation
- Volume ratios

### Volatility Features
- Rolling volatility (10, 20 tick windows)
- Volatility ratio

## Model Architecture

### SimpleLSTM (Classification)
- Bidirectional LSTM layers
- Dropout regularization
- 3-class output: Down, Neutral, Up
- Class-weighted loss for imbalanced data

```python
SimpleLSTM(
    input_size=25,
    hidden_size=64,
    num_layers=2,
    dropout=0.3,
    bidirectional=True
)
```

### LSTMTradingModel (Advanced)
- Attention mechanism
- Residual connections
- Supports regression and classification

## Quick Start

### 1. Install Dependencies

```bash
pip install numpy pandas torch scikit-learn tqdm matplotlib sortedcontainers
```

### 2. Train Model on Your Data

```bash
cd ml
python train_combined.py
```

### 3. Use Pre-trained Model

```python
import torch
from ml.models.lstm_model import SimpleLSTM

# Load model
model = SimpleLSTM(input_size=25, hidden_size=64, num_layers=2)
model.load_state_dict(torch.load('output_combined/best_model.pt'))
model.eval()

# Make predictions
# predictions: 0=Down, 1=Neutral, 2=Up
predictions = model(features_tensor)
```

## Training Results

### Combined Multi-Product Training

- **Products Used**: 15 (LTC-EUR, ZEC-USD, AAVE-EUR, etc.)
- **Total Sequences**: 13,572
- **Test Accuracy**: **84.58%** (vs 33.33% random baseline)
- **Class Accuracies**:
  - Down (0): 64.41%
  - Neutral (1): 88.72%
  - Up (2): 62.42%

### Configuration Used
```python
{
    "hidden_size": 64,
    "num_layers": 2,
    "sequence_length": 15,
    "prediction_horizon": 5,
    "threshold_bps": 2.0
}
```

## Data Format

The system expects Coinbase L2 websocket recordings in NDJSON format:

```json
{
  "raw": "{\"channel\":\"l2_data\",\"timestamp\":\"2025-11-07T17:48:37.959Z\",\"events\":[{\"type\":\"snapshot\",\"product_id\":\"LTC-EUR\",\"updates\":[{\"side\":\"bid\",\"price_level\":\"75.50\",\"new_quantity\":\"10.5\"}]}]}",
  "ts_recv_ns": 1699375717959574117,
  "ws_idx": 0
}
```

## Signal Generation

```python
from ml.signal_generator import TradingSignalGenerator, SignalConfig

config = SignalConfig(
    strong_buy_threshold=3.0,
    buy_threshold=1.0,
    sell_threshold=-1.0,
    strong_sell_threshold=-3.0,
    min_confidence=0.4
)

signal_gen = TradingSignalGenerator(model, feature_extractor, config)
signal = signal_gen.generate_signal(prediction, confidence)
```

## Backtesting

```python
from ml.signal_generator import LSTMBacktester

backtester = LSTMBacktester(
    signal_generator=signal_gen,
    initial_capital=100000.0,
    transaction_cost_bps=2.0,
    max_position_value=50000.0
)

results = backtester.run_backtest(df, sequence_length=15)
print(f"Return: {results['total_return_pct']:.2f}%")
print(f"Sharpe: {results['sharpe_ratio']:.2f}")
```

## Limitations & Notes

1. **Data Quality**: Model performance depends heavily on data quality and recency
2. **Market Regime**: Trained on specific market conditions; may need retraining
3. **Latency**: This is for research/backtesting; live trading requires latency optimization
4. **Class Imbalance**: Most price movements are neutral; model uses class weighting

## Future Improvements

- [ ] Online learning for adaptation to market changes
- [ ] Ensemble of multiple models
- [ ] Cross-validation across time periods
- [ ] Feature importance analysis
- [ ] Integration with live trading infrastructure

## License

Part of HFT_Full_Pipeline project.
