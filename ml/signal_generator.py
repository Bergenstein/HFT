"""
Trading Signal Generator
Converts LSTM model predictions into actionable trading signals
"""

import os
import json
import numpy as np
import pandas as pd
import torch
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass
from enum import Enum

import sys
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/models')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/features')


class Signal(Enum):
    """Trading signal types"""
    STRONG_BUY = 2
    BUY = 1
    HOLD = 0
    SELL = -1
    STRONG_SELL = -2


@dataclass
class SignalConfig:
    """Signal generation configuration"""
    # Thresholds for signal generation (in basis points)
    strong_buy_threshold: float = 10.0
    buy_threshold: float = 5.0
    sell_threshold: float = -5.0
    strong_sell_threshold: float = -10.0
    
    # Confidence thresholds
    min_confidence: float = 0.6  # Minimum confidence to generate signal
    
    # Signal filtering
    use_smoothing: bool = True
    smoothing_window: int = 3
    
    # Position sizing
    max_position: float = 1.0  # Maximum position size (1.0 = 100%)
    position_scale_factor: float = 0.5  # Scale position by prediction confidence


class TradingSignalGenerator:
    """
    Generates trading signals from LSTM model predictions
    """
    
    def __init__(
        self,
        model: torch.nn.Module,
        feature_extractor,
        config: Optional[SignalConfig] = None,
        device: str = 'cpu'
    ):
        self.model = model
        self.feature_extractor = feature_extractor
        self.config = config or SignalConfig()
        self.device = torch.device(device)
        self.model = self.model.to(self.device)
        self.model.eval()
        
        # Feature normalization parameters
        self.feature_means = None
        self.feature_stds = None
        
        # Recent predictions for smoothing
        self.recent_predictions: List[float] = []
    
    def load_normalization_params(self, metadata_path: str):
        """Load feature normalization parameters from training metadata"""
        with open(metadata_path, 'r') as f:
            metadata = json.load(f)
        self.feature_means = pd.Series(metadata['feature_means'])
        self.feature_stds = pd.Series(metadata['feature_stds'])
    
    def set_normalization_params(self, means: Dict, stds: Dict):
        """Set feature normalization parameters"""
        self.feature_means = pd.Series(means)
        self.feature_stds = pd.Series(stds)
    
    def normalize_features(self, features: pd.DataFrame) -> pd.DataFrame:
        """Normalize features using stored parameters"""
        if self.feature_means is None or self.feature_stds is None:
            raise ValueError("Normalization parameters not set")
        
        normalized = (features - self.feature_means) / self.feature_stds
        return normalized.clip(-5.0, 5.0)
    
    def predict(self, features: np.ndarray) -> Tuple[float, float]:
        """
        Make prediction from features
        
        Args:
            features: Feature array of shape (seq_len, num_features)
        
        Returns:
            (prediction, confidence) tuple
        """
        # Ensure correct shape
        if features.ndim == 2:
            features = features[np.newaxis, :]  # Add batch dimension
        
        with torch.no_grad():
            X = torch.FloatTensor(features).to(self.device)
            output = self.model(X)
            prediction = output.squeeze().cpu().numpy()
        
        # Compute confidence (based on prediction magnitude)
        # Higher magnitude = more confident
        confidence = min(abs(prediction) / 20.0, 1.0)  # Normalize to [0, 1]
        
        return float(prediction), float(confidence)
    
    def generate_signal(
        self,
        prediction: float,
        confidence: float
    ) -> Tuple[Signal, float]:
        """
        Generate trading signal from prediction
        
        Args:
            prediction: Model prediction (expected return in bps)
            confidence: Prediction confidence [0, 1]
        
        Returns:
            (signal, position_size) tuple
        """
        cfg = self.config
        
        # Check minimum confidence
        if confidence < cfg.min_confidence:
            return Signal.HOLD, 0.0
        
        # Apply smoothing if enabled
        if cfg.use_smoothing:
            self.recent_predictions.append(prediction)
            if len(self.recent_predictions) > cfg.smoothing_window:
                self.recent_predictions.pop(0)
            prediction = np.mean(self.recent_predictions)
        
        # Determine signal
        if prediction >= cfg.strong_buy_threshold:
            signal = Signal.STRONG_BUY
        elif prediction >= cfg.buy_threshold:
            signal = Signal.BUY
        elif prediction <= cfg.strong_sell_threshold:
            signal = Signal.STRONG_SELL
        elif prediction <= cfg.sell_threshold:
            signal = Signal.SELL
        else:
            signal = Signal.HOLD
        
        # Calculate position size
        if signal == Signal.HOLD:
            position_size = 0.0
        else:
            # Scale position by confidence and prediction magnitude
            base_size = min(abs(prediction) / 20.0, 1.0)
            position_size = base_size * confidence * cfg.position_scale_factor
            position_size = min(position_size, cfg.max_position)
            
            # Make negative for sell signals
            if signal in [Signal.SELL, Signal.STRONG_SELL]:
                position_size = -position_size
        
        return signal, position_size
    
    def process_orderbook_update(
        self,
        df: pd.DataFrame,
        sequence_length: int = 50
    ) -> Optional[Dict]:
        """
        Process order book DataFrame and generate signal
        
        Args:
            df: DataFrame with order book data
            sequence_length: Length of sequence for model
        
        Returns:
            Signal information dictionary or None
        """
        if len(df) < sequence_length + 100:  # Need enough data for features
            return None
        
        # Extract features
        features = self.feature_extractor.extract_features(df)
        
        # Drop NaN and get last sequence
        features = features.dropna()
        if len(features) < sequence_length:
            return None
        
        # Get feature columns (exclude target if present)
        feature_cols = [col for col in features.columns if col != 'target']
        
        # Normalize
        feature_data = features[feature_cols].iloc[-sequence_length:]
        normalized = self.normalize_features(feature_data)
        
        # Make prediction
        prediction, confidence = self.predict(normalized.values)
        
        # Generate signal
        signal, position_size = self.generate_signal(prediction, confidence)
        
        return {
            'timestamp': df.index[-1],
            'prediction_bps': prediction,
            'confidence': confidence,
            'signal': signal.name,
            'signal_value': signal.value,
            'position_size': position_size,
            'mid_price': df['mid_price'].iloc[-1] if 'mid_price' in df else None
        }


@dataclass
class Trade:
    """Represents a single trade"""
    timestamp: pd.Timestamp
    side: str  # 'buy' or 'sell'
    price: float
    quantity: float
    signal: str
    prediction: float
    confidence: float


@dataclass
class Position:
    """Current position state"""
    quantity: float = 0.0
    entry_price: float = 0.0
    realized_pnl: float = 0.0
    unrealized_pnl: float = 0.0


class LSTMBacktester:
    """
    Backtester for LSTM trading signals
    """
    
    def __init__(
        self,
        signal_generator: TradingSignalGenerator,
        initial_capital: float = 100000.0,
        transaction_cost_bps: float = 1.0,  # 1 bp transaction cost
        max_position_value: float = 50000.0
    ):
        self.signal_generator = signal_generator
        self.initial_capital = initial_capital
        self.transaction_cost_bps = transaction_cost_bps
        self.max_position_value = max_position_value
        
        # State
        self.capital = initial_capital
        self.position = Position()
        self.trades: List[Trade] = []
        self.equity_curve: List[Dict] = []
    
    def reset(self):
        """Reset backtester state"""
        self.capital = self.initial_capital
        self.position = Position()
        self.trades = []
        self.equity_curve = []
    
    def execute_trade(
        self,
        timestamp: pd.Timestamp,
        side: str,
        price: float,
        quantity: float,
        signal_info: Dict
    ):
        """Execute a trade"""
        # Apply transaction cost
        cost = abs(quantity * price) * (self.transaction_cost_bps / 10000)
        
        if side == 'buy':
            self.position.quantity += quantity
            self.capital -= quantity * price + cost
        else:  # sell
            self.position.quantity -= quantity
            self.capital += quantity * price - cost
        
        # Update entry price (weighted average)
        if self.position.quantity != 0:
            self.position.entry_price = price
        
        # Record trade
        trade = Trade(
            timestamp=timestamp,
            side=side,
            price=price,
            quantity=quantity,
            signal=signal_info['signal'],
            prediction=signal_info['prediction_bps'],
            confidence=signal_info['confidence']
        )
        self.trades.append(trade)
    
    def update_pnl(self, current_price: float):
        """Update unrealized P&L"""
        if self.position.quantity != 0:
            self.position.unrealized_pnl = (
                (current_price - self.position.entry_price) * self.position.quantity
            )
    
    def get_equity(self, current_price: float) -> float:
        """Get current equity"""
        position_value = self.position.quantity * current_price
        return self.capital + position_value
    
    def run_backtest(
        self,
        df: pd.DataFrame,
        sequence_length: int = 50,
        min_interval: int = 10  # Minimum ticks between trades
    ) -> Dict:
        """
        Run backtest on historical data
        
        Args:
            df: DataFrame with order book data
            sequence_length: Sequence length for model
            min_interval: Minimum ticks between trades
        
        Returns:
            Backtest results
        """
        self.reset()
        
        last_trade_idx = -min_interval
        
        for i in range(sequence_length + 100, len(df)):
            # Get data up to current point
            current_df = df.iloc[:i+1]
            current_price = df['mid_price'].iloc[i]
            timestamp = df.index[i]
            
            # Generate signal
            signal_info = self.signal_generator.process_orderbook_update(
                current_df, 
                sequence_length
            )
            
            if signal_info is None:
                continue
            
            # Update P&L
            self.update_pnl(current_price)
            
            # Record equity
            equity = self.get_equity(current_price)
            self.equity_curve.append({
                'timestamp': timestamp,
                'equity': equity,
                'position': self.position.quantity,
                'signal': signal_info['signal'],
                'prediction': signal_info['prediction_bps']
            })
            
            # Check if we should trade
            if i - last_trade_idx < min_interval:
                continue
            
            target_position = signal_info['position_size'] * self.max_position_value / current_price
            current_position = self.position.quantity
            position_diff = target_position - current_position
            
            # Execute trade if significant position change
            if abs(position_diff * current_price) > 100:  # Min $100 trade
                if position_diff > 0:
                    self.execute_trade(
                        timestamp, 'buy', current_price, position_diff, signal_info
                    )
                else:
                    self.execute_trade(
                        timestamp, 'sell', current_price, abs(position_diff), signal_info
                    )
                last_trade_idx = i
        
        # Close any remaining position
        if self.position.quantity != 0:
            final_price = df['mid_price'].iloc[-1]
            if self.position.quantity > 0:
                self.execute_trade(
                    df.index[-1], 'sell', final_price, 
                    self.position.quantity, {'signal': 'CLOSE', 'prediction_bps': 0, 'confidence': 0}
                )
            else:
                self.execute_trade(
                    df.index[-1], 'buy', final_price,
                    abs(self.position.quantity), {'signal': 'CLOSE', 'prediction_bps': 0, 'confidence': 0}
                )
        
        return self.compute_results()
    
    def compute_results(self) -> Dict:
        """Compute backtest results"""
        if not self.equity_curve:
            return {'error': 'No equity curve data'}
        
        equity_df = pd.DataFrame(self.equity_curve)
        equity_df['timestamp'] = pd.to_datetime(equity_df['timestamp'])
        equity_df = equity_df.set_index('timestamp')
        
        # Calculate returns
        equity_df['returns'] = equity_df['equity'].pct_change()
        
        # Performance metrics
        total_return = (equity_df['equity'].iloc[-1] / self.initial_capital - 1) * 100
        
        # Sharpe ratio (annualized, assuming ~252 trading days)
        returns_std = equity_df['returns'].std()
        if returns_std > 0:
            sharpe = (equity_df['returns'].mean() / returns_std) * np.sqrt(252 * 24 * 60)  # Assuming minute data
        else:
            sharpe = 0
        
        # Max drawdown
        rolling_max = equity_df['equity'].expanding().max()
        drawdown = (equity_df['equity'] - rolling_max) / rolling_max
        max_drawdown = drawdown.min() * 100
        
        # Win rate
        if self.trades:
            # Group trades into round trips
            pnl_per_trade = []
            for i in range(0, len(self.trades) - 1, 2):
                if i + 1 < len(self.trades):
                    entry = self.trades[i]
                    exit_trade = self.trades[i + 1]
                    if entry.side == 'buy':
                        pnl = (exit_trade.price - entry.price) * entry.quantity
                    else:
                        pnl = (entry.price - exit_trade.price) * entry.quantity
                    pnl_per_trade.append(pnl)
            
            if pnl_per_trade:
                win_rate = sum(1 for p in pnl_per_trade if p > 0) / len(pnl_per_trade) * 100
                avg_win = np.mean([p for p in pnl_per_trade if p > 0]) if any(p > 0 for p in pnl_per_trade) else 0
                avg_loss = np.mean([p for p in pnl_per_trade if p < 0]) if any(p < 0 for p in pnl_per_trade) else 0
            else:
                win_rate = 0
                avg_win = 0
                avg_loss = 0
        else:
            win_rate = 0
            avg_win = 0
            avg_loss = 0
        
        return {
            'total_return_pct': total_return,
            'sharpe_ratio': sharpe,
            'max_drawdown_pct': max_drawdown,
            'num_trades': len(self.trades),
            'win_rate_pct': win_rate,
            'avg_win': avg_win,
            'avg_loss': avg_loss,
            'final_equity': equity_df['equity'].iloc[-1],
            'initial_capital': self.initial_capital,
            'equity_curve': equity_df
        }


def load_trained_model(
    checkpoint_path: str,
    metadata_path: str,
    device: str = 'cpu'
) -> Tuple[torch.nn.Module, Dict]:
    """Load a trained model from checkpoint"""
    from lstm_model import create_model
    
    # Load metadata
    with open(metadata_path, 'r') as f:
        metadata = json.load(f)
    
    config = metadata['config']
    
    # Create model
    input_size = len(metadata['feature_names'])
    model_config = {
        'hidden_size': config['hidden_size'],
        'num_layers': config['num_layers'],
        'dropout': config['dropout'],
        'bidirectional': config.get('bidirectional', True),
        'use_attention': config.get('use_attention', True),
        'output_type': config['output_type']
    }
    model = create_model(config['model_type'], input_size, model_config)
    
    # Load weights
    checkpoint = torch.load(checkpoint_path, map_location=device)
    model.load_state_dict(checkpoint['model_state_dict'])
    model.eval()
    
    return model, metadata


if __name__ == "__main__":
    print("Signal generator module loaded successfully")
    print("Use load_trained_model() to load a trained model")
    print("Use TradingSignalGenerator for real-time signal generation")
    print("Use LSTMBacktester for backtesting")
