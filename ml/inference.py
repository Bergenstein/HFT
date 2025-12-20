#!/usr/bin/env python3
"""
LSTM Trading Inference
Load trained model and make predictions on new data
"""

import os
import sys
import json
import torch
import numpy as np
import pandas as pd
from typing import Dict, Optional, Tuple

sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml')
from l2_data_loader import L2DataLoader, discover_products


class SimpleLSTM(torch.nn.Module):
    """Simple LSTM for direction prediction (must match training architecture)"""
    
    def __init__(self, input_size: int, hidden_size: int = 64, num_layers: int = 2, dropout: float = 0.3):
        super().__init__()
        
        self.lstm = torch.nn.LSTM(
            input_size=input_size,
            hidden_size=hidden_size,
            num_layers=num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0,
            bidirectional=True
        )
        
        self.fc = torch.nn.Sequential(
            torch.nn.Linear(hidden_size * 2, hidden_size),
            torch.nn.ReLU(),
            torch.nn.Dropout(dropout),
            torch.nn.Linear(hidden_size, 3)
        )
    
    def forward(self, x):
        lstm_out, _ = self.lstm(x)
        out = lstm_out[:, -1, :]
        return self.fc(out)


def extract_features(df: pd.DataFrame) -> pd.DataFrame:
    """Extract features from order book data (must match training)"""
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


class TradingPredictor:
    """
    Load trained LSTM model and make predictions
    """
    
    def __init__(
        self,
        model_path: str = '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/output_combined/best_model.pt',
        metadata_path: str = '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/output_combined/metadata.json',
        device: str = 'cpu'
    ):
        self.device = torch.device(device)
        
        # Load metadata
        with open(metadata_path, 'r') as f:
            self.metadata = json.load(f)
        
        self.config = self.metadata['config']
        self.sequence_length = self.config['sequence_length']
        
        # Create model
        input_size = 25  # Number of features
        self.model = SimpleLSTM(
            input_size=input_size,
            hidden_size=self.config['hidden_size'],
            num_layers=self.config['num_layers'],
            dropout=0.3
        )
        
        # Load weights
        self.model.load_state_dict(torch.load(model_path, map_location=device))
        self.model = self.model.to(self.device)
        self.model.eval()
        
        # Feature buffer for streaming
        self.feature_buffer = []
        
        print(f"Model loaded from {model_path}")
        print(f"Config: {self.config}")
    
    def prepare_features(self, df: pd.DataFrame) -> Optional[np.ndarray]:
        """Prepare features from order book DataFrame"""
        features = extract_features(df)
        features = features.dropna()
        
        if len(features) < self.sequence_length:
            return None
        
        # Get feature columns (same order as training)
        feature_cols = list(features.columns)
        
        # Normalize (using simple z-score per sample)
        values = features[feature_cols].values
        mean = np.mean(values, axis=0, keepdims=True)
        std = np.std(values, axis=0, keepdims=True) + 1e-8
        normalized = (values - mean) / std
        normalized = np.clip(normalized, -5, 5)
        
        # Return last sequence
        return normalized[-self.sequence_length:]
    
    def predict(self, features: np.ndarray) -> Tuple[int, np.ndarray]:
        """
        Make prediction
        
        Returns:
            (predicted_class, probabilities)
            class: 0=Down, 1=Neutral, 2=Up
        """
        if features.ndim == 2:
            features = features[np.newaxis, :]
        
        with torch.no_grad():
            X = torch.FloatTensor(features).to(self.device)
            output = self.model(X)
            probs = torch.softmax(output, dim=1).cpu().numpy()[0]
            pred = np.argmax(probs)
        
        return int(pred), probs
    
    def predict_from_df(self, df: pd.DataFrame) -> Optional[Dict]:
        """
        Make prediction from order book DataFrame
        
        Returns dict with prediction info or None if not enough data
        """
        features = self.prepare_features(df)
        
        if features is None:
            return None
        
        pred, probs = self.predict(features)
        
        signal_map = {0: 'DOWN', 1: 'NEUTRAL', 2: 'UP'}
        
        return {
            'prediction': pred,
            'signal': signal_map[pred],
            'probabilities': {
                'down': float(probs[0]),
                'neutral': float(probs[1]),
                'up': float(probs[2])
            },
            'confidence': float(probs.max()),
            'mid_price': float(df['mid_price'].iloc[-1]) if 'mid_price' in df else None,
            'timestamp': str(df.index[-1])
        }
    
    def stream_predict(self, new_features: np.ndarray) -> Optional[Dict]:
        """
        Streaming prediction - accumulate features and predict when ready
        
        Args:
            new_features: Single feature row (1D array)
        
        Returns:
            Prediction dict or None if not enough data yet
        """
        self.feature_buffer.append(new_features)
        
        # Keep only necessary history
        if len(self.feature_buffer) > self.sequence_length * 2:
            self.feature_buffer = self.feature_buffer[-self.sequence_length:]
        
        if len(self.feature_buffer) < self.sequence_length:
            return None
        
        features = np.array(self.feature_buffer[-self.sequence_length:])
        pred, probs = self.predict(features)
        
        signal_map = {0: 'DOWN', 1: 'NEUTRAL', 2: 'UP'}
        
        return {
            'prediction': pred,
            'signal': signal_map[pred],
            'confidence': float(probs.max()),
            'probabilities': {
                'down': float(probs[0]),
                'neutral': float(probs[1]),
                'up': float(probs[2])
            }
        }


def demo_inference():
    """Demo inference on sample data"""
    
    print("="*60)
    print("LSTM Trading Model Inference Demo")
    print("="*60)
    
    # Load predictor
    predictor = TradingPredictor()
    
    # Load some sample data
    data_dir = "/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/data"
    loader = L2DataLoader(data_dir)
    
    products = discover_products(data_dir)
    product = list(products.keys())[0]
    print(f"\nLoading sample data for {product}...")
    
    data = loader.load_and_process(products=[product])
    dfs = loader.to_dataframes(data)
    df = dfs[product]
    
    print(f"Data shape: {df.shape}")
    
    # Make prediction on latest data
    result = predictor.predict_from_df(df)
    
    if result:
        print("\n" + "-"*40)
        print("PREDICTION")
        print("-"*40)
        print(f"Signal:     {result['signal']}")
        print(f"Confidence: {result['confidence']*100:.1f}%")
        print(f"Mid Price:  {result['mid_price']:.4f}")
        print(f"Probabilities:")
        print(f"  Down:    {result['probabilities']['down']*100:.1f}%")
        print(f"  Neutral: {result['probabilities']['neutral']*100:.1f}%")
        print(f"  Up:      {result['probabilities']['up']*100:.1f}%")
    else:
        print("Not enough data for prediction")
    
    # Simulate streaming predictions
    print("\n" + "-"*40)
    print("STREAMING PREDICTIONS (Last 10)")
    print("-"*40)
    
    features = extract_features(df).dropna()
    predictor2 = TradingPredictor()
    
    for i, (idx, row) in enumerate(features.iloc[-50:].iterrows()):
        result = predictor2.stream_predict(row.values)
        if result and i >= 40:  # Only show last 10
            print(f"{idx}: {result['signal']:8s} (conf: {result['confidence']*100:.1f}%)")


if __name__ == "__main__":
    demo_inference()
