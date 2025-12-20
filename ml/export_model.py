#!/usr/bin/env python3
"""
Export LSTM Model to Lightweight Binary Format for C++ Hot Path

This module exports the trained PyTorch LSTM model to:
1. Raw binary weights file (.bin) - for fastest C++ loading
2. JSON metadata file (.json) - model architecture and normalization params
3. Optional ONNX format (.onnx) - for ONNX Runtime inference

The binary format is designed for zero-copy memory mapping in C++.
"""

import os
import sys
import json
import struct
import numpy as np
import torch
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass, asdict

sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml')
sys.path.insert(0, '/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/models')


@dataclass
class LSTMExportConfig:
    """Configuration for exported model"""
    # Model architecture
    input_size: int
    hidden_size: int
    num_layers: int
    bidirectional: bool
    use_attention: bool
    output_type: str  # 'regression' or 'classification'
    
    # Sequence config
    sequence_length: int
    
    # Feature names (for documentation)
    feature_names: List[str]
    
    # Normalization parameters
    feature_means: List[float]
    feature_stds: List[float]
    
    # Signal thresholds
    buy_threshold: float = 5.0
    sell_threshold: float = -5.0
    
    # Version info
    version: str = "1.0"


def export_to_binary(
    checkpoint_path: str,
    metadata_path: str,
    output_dir: str,
    model_name: str = "lstm_trading"
) -> Dict[str, str]:
    """
    Export PyTorch model to lightweight binary format for C++ loading.
    
    Binary format structure:
    - Header (32 bytes):
        - Magic number (4 bytes): "LSTM"
        - Version (4 bytes): uint32
        - Input size (4 bytes): uint32
        - Hidden size (4 bytes): uint32
        - Num layers (4 bytes): uint32
        - Flags (4 bytes): bidirectional(1), attention(1), reserved(30)
        - Num weights (4 bytes): uint32
        - Reserved (4 bytes)
    - Weight data: float32 array, row-major
    
    Returns:
        Dict with paths to exported files
    """
    from lstm_model import create_model
    
    os.makedirs(output_dir, exist_ok=True)
    
    # Load metadata
    with open(metadata_path, 'r') as f:
        metadata = json.load(f)
    
    config = metadata['config']
    
    # Create and load model
    input_size = len(metadata['feature_names'])
    model_config = {
        'hidden_size': config['hidden_size'],
        'num_layers': config['num_layers'],
        'dropout': 0.0,  # No dropout for inference
        'bidirectional': config.get('bidirectional', True),
        'use_attention': config.get('use_attention', True),
        'output_type': config['output_type']
    }
    
    model = create_model(config['model_type'], input_size, model_config)
    checkpoint = torch.load(checkpoint_path, map_location='cpu')
    model.load_state_dict(checkpoint['model_state_dict'])
    model.eval()
    
    # Export config
    export_config = LSTMExportConfig(
        input_size=input_size,
        hidden_size=config['hidden_size'],
        num_layers=config['num_layers'],
        bidirectional=config.get('bidirectional', True),
        use_attention=config.get('use_attention', True),
        output_type=config['output_type'],
        sequence_length=config['sequence_length'],
        feature_names=metadata['feature_names'],
        feature_means=list(metadata['feature_means'].values()),
        feature_stds=list(metadata['feature_stds'].values())
    )
    
    # === Export weights to binary ===
    weights_path = os.path.join(output_dir, f"{model_name}_weights.bin")
    
    # Collect all weights as flat array
    all_weights = []
    weight_info = []
    
    for name, param in model.named_parameters():
        w = param.detach().cpu().numpy().astype(np.float32)
        weight_info.append({
            'name': name,
            'shape': list(w.shape),
            'offset': len(all_weights),
            'size': w.size
        })
        all_weights.extend(w.flatten().tolist())
    
    # Write binary file
    with open(weights_path, 'wb') as f:
        # Header
        f.write(b'LSTM')  # Magic
        f.write(struct.pack('<I', 1))  # Version
        f.write(struct.pack('<I', input_size))
        f.write(struct.pack('<I', config['hidden_size']))
        f.write(struct.pack('<I', config['num_layers']))
        
        flags = (1 if config.get('bidirectional', True) else 0) | \
                (2 if config.get('use_attention', True) else 0)
        f.write(struct.pack('<I', flags))
        
        f.write(struct.pack('<I', len(all_weights)))
        f.write(struct.pack('<I', 0))  # Reserved
        
        # Weights
        weights_array = np.array(all_weights, dtype=np.float32)
        f.write(weights_array.tobytes())
    
    # === Export config JSON ===
    config_path = os.path.join(output_dir, f"{model_name}_config.json")
    
    export_data = {
        'model': asdict(export_config),
        'weights': weight_info,
        'total_weights': len(all_weights),
        'binary_file': f"{model_name}_weights.bin"
    }
    
    with open(config_path, 'w') as f:
        json.dump(export_data, f, indent=2)
    
    # === Export simplified inference weights ===
    # For the C++ hot path, we need a simplified forward pass
    # Export just the essential matrices for a minimal LSTM
    
    simple_weights_path = os.path.join(output_dir, f"{model_name}_simple.bin")
    simple_config = export_simplified_lstm(model, export_config, simple_weights_path)
    
    simple_config_path = os.path.join(output_dir, f"{model_name}_simple.json")
    with open(simple_config_path, 'w') as f:
        json.dump(simple_config, f, indent=2)
    
    print(f"Exported model to {output_dir}:")
    print(f"  - {weights_path} ({os.path.getsize(weights_path):,} bytes)")
    print(f"  - {config_path}")
    print(f"  - {simple_weights_path} ({os.path.getsize(simple_weights_path):,} bytes)")
    print(f"  - {simple_config_path}")
    
    return {
        'weights': weights_path,
        'config': config_path,
        'simple_weights': simple_weights_path,
        'simple_config': simple_config_path
    }


def export_simplified_lstm(
    model: torch.nn.Module,
    config: LSTMExportConfig,
    output_path: str
) -> Dict:
    """
    Export a simplified version of LSTM weights for fast C++ inference.
    
    For HFT, we use a simplified single-step update approach:
    - Pre-compute input projection
    - Store LSTM weights in optimized layout
    - Store attention and output weights
    
    Returns config dict with weight layout info
    """
    state_dict = model.state_dict()
    
    weights = {}
    
    # Input projection: Linear(input_size -> hidden_size)
    weights['input_proj_w'] = state_dict['input_projection.0.weight'].cpu().numpy()
    weights['input_proj_b'] = state_dict['input_projection.0.bias'].cpu().numpy()
    weights['input_ln_w'] = state_dict['input_projection.1.weight'].cpu().numpy()
    weights['input_ln_b'] = state_dict['input_projection.1.bias'].cpu().numpy()
    
    # LSTM weights - PyTorch stores as [weight_ih, weight_hh, bias_ih, bias_hh]
    # For each layer and direction
    num_directions = 2 if config.bidirectional else 1
    
    for layer in range(config.num_layers):
        for direction in range(num_directions):
            suffix = f'_l{layer}' + ('_reverse' if direction == 1 else '')
            
            # Weight matrices: (4*hidden, input/hidden)
            weights[f'lstm_ih{suffix}'] = state_dict[f'lstm.weight_ih{suffix}'].cpu().numpy()
            weights[f'lstm_hh{suffix}'] = state_dict[f'lstm.weight_hh{suffix}'].cpu().numpy()
            weights[f'lstm_bih{suffix}'] = state_dict[f'lstm.bias_ih{suffix}'].cpu().numpy()
            weights[f'lstm_bhh{suffix}'] = state_dict[f'lstm.bias_hh{suffix}'].cpu().numpy()
    
    # Attention weights (if used)
    if config.use_attention:
        weights['attn_w1'] = state_dict['attention.attention.0.weight'].cpu().numpy()
        weights['attn_b1'] = state_dict['attention.attention.0.bias'].cpu().numpy()
        weights['attn_w2'] = state_dict['attention.attention.2.weight'].cpu().numpy()
        weights['attn_b2'] = state_dict['attention.attention.2.bias'].cpu().numpy()
    
    # FC layers
    weights['fc1_w'] = state_dict['fc.0.weight'].cpu().numpy()
    weights['fc1_b'] = state_dict['fc.0.bias'].cpu().numpy()
    weights['fc1_ln_w'] = state_dict['fc.1.weight'].cpu().numpy()
    weights['fc1_ln_b'] = state_dict['fc.1.bias'].cpu().numpy()
    
    weights['fc2_w'] = state_dict['fc.4.weight'].cpu().numpy()
    weights['fc2_b'] = state_dict['fc.4.bias'].cpu().numpy()
    weights['fc2_ln_w'] = state_dict['fc.5.weight'].cpu().numpy()
    weights['fc2_ln_b'] = state_dict['fc.5.bias'].cpu().numpy()
    
    # Output layer
    weights['output_w'] = state_dict['output.weight'].cpu().numpy()
    weights['output_b'] = state_dict['output.bias'].cpu().numpy()
    
    # Feature normalization
    weights['feature_means'] = np.array(config.feature_means, dtype=np.float32)
    weights['feature_stds'] = np.array(config.feature_stds, dtype=np.float32)
    
    # Write to binary
    weight_layout = {}
    offset = 0
    
    with open(output_path, 'wb') as f:
        # Header
        f.write(b'LSIM')  # Magic for simplified format
        f.write(struct.pack('<I', 1))  # Version
        f.write(struct.pack('<I', config.input_size))
        f.write(struct.pack('<I', config.hidden_size))
        f.write(struct.pack('<I', config.num_layers))
        f.write(struct.pack('<I', config.sequence_length))
        flags = (1 if config.bidirectional else 0) | (2 if config.use_attention else 0)
        f.write(struct.pack('<I', flags))
        f.write(struct.pack('<I', len(weights)))  # Num weight arrays
        
        # Write each weight array
        for name, arr in weights.items():
            arr = arr.astype(np.float32).flatten()
            weight_layout[name] = {
                'offset': offset,
                'size': arr.size,
                'shape': list(weights[name].shape)
            }
            f.write(arr.tobytes())
            offset += arr.size * 4
    
    return {
        'input_size': config.input_size,
        'hidden_size': config.hidden_size,
        'num_layers': config.num_layers,
        'sequence_length': config.sequence_length,
        'bidirectional': config.bidirectional,
        'use_attention': config.use_attention,
        'feature_names': config.feature_names,
        'weights': weight_layout,
        'header_size': 32,
        'total_bytes': offset + 32
    }


def export_to_onnx(
    checkpoint_path: str,
    metadata_path: str,
    output_path: str
) -> str:
    """Export model to ONNX format for ONNX Runtime inference"""
    from lstm_model import create_model
    
    # Load metadata and model
    with open(metadata_path, 'r') as f:
        metadata = json.load(f)
    
    config = metadata['config']
    input_size = len(metadata['feature_names'])
    
    model_config = {
        'hidden_size': config['hidden_size'],
        'num_layers': config['num_layers'],
        'dropout': 0.0,
        'bidirectional': config.get('bidirectional', True),
        'use_attention': config.get('use_attention', True),
        'output_type': config['output_type']
    }
    
    model = create_model(config['model_type'], input_size, model_config)
    checkpoint = torch.load(checkpoint_path, map_location='cpu')
    model.load_state_dict(checkpoint['model_state_dict'])
    model.eval()
    
    # Create dummy input
    seq_len = config['sequence_length']
    dummy_input = torch.randn(1, seq_len, input_size)
    
    # Export to ONNX
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=['features'],
        output_names=['prediction'],
        dynamic_axes={
            'features': {0: 'batch_size'},
            'prediction': {0: 'batch_size'}
        },
        opset_version=11
    )
    
    print(f"Exported ONNX model to {output_path}")
    return output_path


if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description='Export LSTM model for C++ hot path')
    parser.add_argument('--checkpoint', type=str, 
                        default='/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/output/checkpoints/best_model.pt')
    parser.add_argument('--metadata', type=str,
                        default='/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/output/checkpoints/training_metadata.json')
    parser.add_argument('--output-dir', type=str,
                        default='/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/ml/exported')
    parser.add_argument('--name', type=str, default='lstm_trading')
    parser.add_argument('--onnx', action='store_true', help='Also export ONNX format')
    
    args = parser.parse_args()
    
    # Export binary format
    paths = export_to_binary(
        args.checkpoint,
        args.metadata,
        args.output_dir,
        args.name
    )
    
    # Optionally export ONNX
    if args.onnx:
        onnx_path = os.path.join(args.output_dir, f"{args.name}.onnx")
        export_to_onnx(args.checkpoint, args.metadata, onnx_path)
    
    print("\nExport complete!")
