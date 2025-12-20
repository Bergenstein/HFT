"""
Feature Extraction for LSTM Trading Model
Extracts meaningful features from L2 order book data for LSTM prediction
"""

import numpy as np
import pandas as pd
from typing import List, Dict, Optional, Tuple
from dataclasses import dataclass


@dataclass
class FeatureConfig:
    """Configuration for feature extraction"""
    # Lookback windows for rolling features
    short_window: int = 10
    medium_window: int = 50
    long_window: int = 200
    
    # Depth levels for order book features
    depth_levels: int = 5
    
    # Target configuration
    prediction_horizon: int = 10  # Predict price change N ticks ahead
    target_threshold_bps: float = 5.0  # Threshold for classification (basis points)
    
    # Feature normalization
    normalize: bool = True
    clip_std: float = 5.0  # Clip outliers beyond N standard deviations


class FeatureExtractor:
    """
    Extracts features from L2 order book data for LSTM model
    
    Features include:
    - Price features: returns, log returns, price momentum
    - Spread features: bid-ask spread, spread changes
    - Volume/depth features: bid/ask volumes, depth imbalance
    - Microstructure features: microprice, order flow imbalance
    - Technical features: moving averages, volatility
    """
    
    def __init__(self, config: Optional[FeatureConfig] = None):
        self.config = config or FeatureConfig()
        self.feature_names: List[str] = []
    
    def extract_features(self, df: pd.DataFrame) -> pd.DataFrame:
        """
        Extract all features from order book DataFrame
        
        Args:
            df: DataFrame with columns: mid_price, best_bid, best_ask, 
                best_bid_qty, best_ask_qty, spread, microprice, 
                top_imbalance, depth_imbalance_5, bid_depth_5, ask_depth_5
        
        Returns:
            DataFrame with all features
        """
        features = pd.DataFrame(index=df.index)
        
        # === Price Features ===
        features['return_1'] = df['mid_price'].pct_change()
        features['return_5'] = df['mid_price'].pct_change(5)
        features['return_10'] = df['mid_price'].pct_change(10)
        features['log_return_1'] = np.log(df['mid_price']).diff()
        features['log_return_5'] = np.log(df['mid_price']).diff(5)
        
        # Price momentum
        features['momentum_short'] = df['mid_price'].pct_change(self.config.short_window)
        features['momentum_medium'] = df['mid_price'].pct_change(self.config.medium_window)
        
        # Price relative to moving averages
        ma_short = df['mid_price'].rolling(self.config.short_window).mean()
        ma_medium = df['mid_price'].rolling(self.config.medium_window).mean()
        ma_long = df['mid_price'].rolling(self.config.long_window).mean()
        
        features['price_ma_ratio_short'] = df['mid_price'] / ma_short - 1
        features['price_ma_ratio_medium'] = df['mid_price'] / ma_medium - 1
        features['price_ma_ratio_long'] = df['mid_price'] / ma_long - 1
        
        # === Volatility Features ===
        features['volatility_short'] = features['log_return_1'].rolling(self.config.short_window).std()
        features['volatility_medium'] = features['log_return_1'].rolling(self.config.medium_window).std()
        features['volatility_ratio'] = features['volatility_short'] / features['volatility_medium']
        
        # High-low range proxy (using bid-ask)
        features['range_proxy'] = (df['best_ask'] - df['best_bid']) / df['mid_price']
        features['range_proxy_ma'] = features['range_proxy'].rolling(self.config.short_window).mean()
        
        # === Spread Features ===
        features['spread_bps'] = df['spread_bps'] if 'spread_bps' in df.columns else \
                                 (df['spread'] / df['mid_price'] * 10000)
        features['spread_change'] = features['spread_bps'].diff()
        features['spread_ma_ratio'] = features['spread_bps'] / features['spread_bps'].rolling(self.config.medium_window).mean()
        features['spread_zscore'] = (features['spread_bps'] - features['spread_bps'].rolling(self.config.medium_window).mean()) / \
                                     features['spread_bps'].rolling(self.config.medium_window).std()
        
        # === Order Book Imbalance Features ===
        features['top_imbalance'] = df['top_imbalance']
        features['top_imbalance_ma'] = df['top_imbalance'].rolling(self.config.short_window).mean()
        features['top_imbalance_change'] = df['top_imbalance'].diff()
        
        if 'depth_imbalance_5' in df.columns:
            features['depth_imbalance'] = df['depth_imbalance_5']
            features['depth_imbalance_ma'] = df['depth_imbalance_5'].rolling(self.config.short_window).mean()
            features['depth_imbalance_change'] = df['depth_imbalance_5'].diff()
        
        # === Volume/Depth Features ===
        if 'bid_depth_5' in df.columns and 'ask_depth_5' in df.columns:
            total_depth = df['bid_depth_5'] + df['ask_depth_5']
            features['total_depth'] = total_depth
            features['total_depth_change'] = total_depth.pct_change()
            features['total_depth_ma_ratio'] = total_depth / total_depth.rolling(self.config.medium_window).mean()
            
            features['bid_depth_ratio'] = df['bid_depth_5'] / total_depth
            features['ask_depth_ratio'] = df['ask_depth_5'] / total_depth
            
        features['bid_qty_change'] = df['best_bid_qty'].pct_change()
        features['ask_qty_change'] = df['best_ask_qty'].pct_change()
        features['qty_ratio'] = df['best_bid_qty'] / df['best_ask_qty']
        features['qty_ratio_change'] = features['qty_ratio'].pct_change()
        
        # === Microprice Features ===
        if 'microprice' in df.columns:
            features['microprice_mid_diff'] = (df['microprice'] - df['mid_price']) / df['mid_price'] * 10000
            features['microprice_mid_diff_ma'] = features['microprice_mid_diff'].rolling(self.config.short_window).mean()
            features['microprice_return'] = df['microprice'].pct_change()
        
        # === Order Flow Imbalance (OFI) approximation ===
        # Approximate OFI using changes in bid/ask quantities
        bid_change = df['best_bid_qty'].diff()
        ask_change = df['best_ask_qty'].diff()
        features['ofi_approx'] = bid_change - ask_change
        features['ofi_approx_ma'] = features['ofi_approx'].rolling(self.config.short_window).mean()
        features['ofi_cumsum_short'] = features['ofi_approx'].rolling(self.config.short_window).sum()
        
        # === Cross-sectional features ===
        # Bid-ask pressure
        features['pressure'] = (df['best_bid_qty'] - df['best_ask_qty']) / (df['best_bid_qty'] + df['best_ask_qty'])
        features['pressure_ma'] = features['pressure'].rolling(self.config.short_window).mean()
        features['pressure_momentum'] = features['pressure'] - features['pressure'].shift(self.config.short_window)
        
        # === Lagged features (for sequence modeling) ===
        for lag in [1, 2, 3, 5]:
            features[f'return_lag_{lag}'] = features['return_1'].shift(lag)
            features[f'imbalance_lag_{lag}'] = features['top_imbalance'].shift(lag)
        
        # Store feature names
        self.feature_names = [col for col in features.columns if col != 'target']
        
        return features
    
    def create_target(
        self, 
        df: pd.DataFrame, 
        features: pd.DataFrame,
        target_type: str = 'regression'
    ) -> pd.Series:
        """
        Create prediction target
        
        Args:
            df: Original DataFrame with mid_price
            features: Features DataFrame
            target_type: 'regression' for price change, 'classification' for direction
        
        Returns:
            Target series
        """
        horizon = self.config.prediction_horizon
        
        # Future return
        future_return = df['mid_price'].pct_change(horizon).shift(-horizon)
        future_return_bps = future_return * 10000
        
        if target_type == 'regression':
            return future_return_bps
        elif target_type == 'classification':
            # 3-class: -1 (down), 0 (neutral), 1 (up)
            threshold = self.config.target_threshold_bps
            target = pd.Series(0, index=df.index)
            target[future_return_bps > threshold] = 1
            target[future_return_bps < -threshold] = -1
            return target
        elif target_type == 'binary':
            # Binary: 0 (down/neutral), 1 (up)
            return (future_return > 0).astype(int)
        else:
            raise ValueError(f"Unknown target type: {target_type}")
    
    def normalize_features(self, features: pd.DataFrame, fit: bool = True) -> pd.DataFrame:
        """
        Normalize features using z-score normalization with outlier clipping
        """
        if fit:
            self.feature_means = features.mean()
            self.feature_stds = features.std()
        
        normalized = (features - self.feature_means) / self.feature_stds
        
        # Clip outliers
        if self.config.clip_std:
            normalized = normalized.clip(-self.config.clip_std, self.config.clip_std)
        
        return normalized
    
    def prepare_lstm_data(
        self,
        df: pd.DataFrame,
        sequence_length: int = 50,
        target_type: str = 'regression',
        train_ratio: float = 0.7,
        val_ratio: float = 0.15
    ) -> Dict:
        """
        Prepare data for LSTM training
        
        Returns:
            Dictionary with train/val/test splits of X (sequences) and y (targets)
        """
        # Extract features
        features = self.extract_features(df)
        
        # Create target
        target = self.create_target(df, features, target_type)
        features['target'] = target
        
        # Drop NaN rows
        valid_mask = ~(features.isna().any(axis=1))
        features = features[valid_mask]
        
        if len(features) < sequence_length + 100:
            raise ValueError(f"Not enough data: {len(features)} rows after cleaning")
        
        # Split into train/val/test (time-based split)
        n = len(features)
        train_end = int(n * train_ratio)
        val_end = int(n * (train_ratio + val_ratio))
        
        train_data = features.iloc[:train_end]
        val_data = features.iloc[train_end:val_end]
        test_data = features.iloc[val_end:]
        
        # Normalize using training data statistics
        feature_cols = [col for col in features.columns if col != 'target']
        
        train_features = train_data[feature_cols]
        self.feature_means = train_features.mean()
        self.feature_stds = train_features.std().replace(0, 1)
        
        def normalize(data):
            return (data - self.feature_means) / self.feature_stds
        
        train_X = normalize(train_data[feature_cols]).clip(-self.config.clip_std, self.config.clip_std)
        val_X = normalize(val_data[feature_cols]).clip(-self.config.clip_std, self.config.clip_std)
        test_X = normalize(test_data[feature_cols]).clip(-self.config.clip_std, self.config.clip_std)
        
        train_y = train_data['target']
        val_y = val_data['target']
        test_y = test_data['target']
        
        # Create sequences
        def create_sequences(X, y, seq_len):
            X_seq, y_seq = [], []
            X_values = X.values
            y_values = y.values
            
            for i in range(len(X) - seq_len):
                X_seq.append(X_values[i:i+seq_len])
                y_seq.append(y_values[i+seq_len-1])  # Target at end of sequence
            
            return np.array(X_seq), np.array(y_seq)
        
        X_train, y_train = create_sequences(train_X, train_y, sequence_length)
        X_val, y_val = create_sequences(val_X, val_y, sequence_length)
        X_test, y_test = create_sequences(test_X, test_y, sequence_length)
        
        return {
            'X_train': X_train,
            'y_train': y_train,
            'X_val': X_val,
            'y_val': y_val,
            'X_test': X_test,
            'y_test': y_test,
            'feature_names': feature_cols,
            'feature_means': self.feature_means.to_dict(),
            'feature_stds': self.feature_stds.to_dict(),
            'train_timestamps': train_data.index[sequence_length:].tolist(),
            'val_timestamps': val_data.index[sequence_length:].tolist(),
            'test_timestamps': test_data.index[sequence_length:].tolist(),
        }


def compute_feature_importance(
    features: pd.DataFrame,
    target: pd.Series,
    method: str = 'correlation'
) -> pd.Series:
    """
    Compute feature importance scores
    """
    if method == 'correlation':
        correlations = features.corrwith(target).abs()
        return correlations.sort_values(ascending=False)
    else:
        raise ValueError(f"Unknown method: {method}")


if __name__ == "__main__":
    # Test feature extraction
    import sys
    sys.path.append('/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml')
    from l2_data_loader import L2DataLoader, discover_products
    
    data_dir = "/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/data"
    
    print("Loading data...")
    loader = L2DataLoader(data_dir)
    
    # Find products with most data
    products = discover_products(data_dir)
    if not products:
        print("No products found!")
        exit(1)
    
    # Use product with most updates
    top_products = list(products.keys())[:3]
    print(f"Top products: {top_products}")
    
    for product in top_products:
        print(f"\n=== Processing {product} ===")
        
        data = loader.load_and_process(products=[product])
        dfs = loader.to_dataframes(data)
        
        if product not in dfs:
            print(f"  No data for {product}")
            continue
        
        df = dfs[product]
        print(f"  Raw data shape: {df.shape}")
        
        if len(df) < 300:
            print(f"  Not enough data (need at least 300 rows)")
            continue
        
        # Extract features
        config = FeatureConfig(
            short_window=10,
            medium_window=50,
            long_window=100,
            prediction_horizon=10
        )
        extractor = FeatureExtractor(config)
        
        try:
            lstm_data = extractor.prepare_lstm_data(
                df, 
                sequence_length=50,
                target_type='regression'
            )
            
            print(f"  Train shape: {lstm_data['X_train'].shape}")
            print(f"  Val shape: {lstm_data['X_val'].shape}")
            print(f"  Test shape: {lstm_data['X_test'].shape}")
            print(f"  Num features: {len(lstm_data['feature_names'])}")
            print(f"  Features: {lstm_data['feature_names'][:10]}...")
            
        except Exception as e:
            print(f"  Error: {e}")
