"""
Feature extraction module for LSTM trading
"""

from .feature_extractor import FeatureExtractor, FeatureConfig, compute_feature_importance

__all__ = ['FeatureExtractor', 'FeatureConfig', 'compute_feature_importance']
