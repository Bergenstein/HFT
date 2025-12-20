"""
LSTM Model for Trading Signal Prediction
Multi-layer LSTM with attention mechanism for L2 order book data
"""

import torch
import torch.nn as nn
import torch.nn.functional as F
from typing import Optional, Tuple, Dict
import numpy as np


class Attention(nn.Module):
    """
    Attention mechanism for LSTM outputs
    Helps the model focus on important time steps
    """
    
    def __init__(self, hidden_size: int, attention_size: int = 64):
        super().__init__()
        self.attention = nn.Sequential(
            nn.Linear(hidden_size, attention_size),
            nn.Tanh(),
            nn.Linear(attention_size, 1)
        )
    
    def forward(self, lstm_output: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor]:
        """
        Args:
            lstm_output: (batch, seq_len, hidden_size)
        
        Returns:
            context: (batch, hidden_size) - weighted sum of LSTM outputs
            attention_weights: (batch, seq_len) - attention weights
        """
        # Compute attention scores
        scores = self.attention(lstm_output).squeeze(-1)  # (batch, seq_len)
        attention_weights = F.softmax(scores, dim=1)  # (batch, seq_len)
        
        # Weighted sum
        context = torch.bmm(attention_weights.unsqueeze(1), lstm_output).squeeze(1)  # (batch, hidden_size)
        
        return context, attention_weights


class LSTMTradingModel(nn.Module):
    """
    LSTM model for trading signal prediction
    
    Architecture:
    - Input projection layer
    - Multi-layer bidirectional LSTM
    - Attention mechanism
    - Fully connected layers with dropout
    - Output layer (regression or classification)
    """
    
    def __init__(
        self,
        input_size: int,
        hidden_size: int = 128,
        num_layers: int = 2,
        dropout: float = 0.2,
        bidirectional: bool = True,
        use_attention: bool = True,
        output_type: str = 'regression',  # 'regression', 'classification', 'binary'
        num_classes: int = 3  # For classification
    ):
        super().__init__()
        
        self.input_size = input_size
        self.hidden_size = hidden_size
        self.num_layers = num_layers
        self.bidirectional = bidirectional
        self.use_attention = use_attention
        self.output_type = output_type
        self.num_classes = num_classes
        
        # Direction multiplier for bidirectional
        self.num_directions = 2 if bidirectional else 1
        
        # Input projection
        self.input_projection = nn.Sequential(
            nn.Linear(input_size, hidden_size),
            nn.LayerNorm(hidden_size),
            nn.ReLU(),
            nn.Dropout(dropout)
        )
        
        # LSTM layers
        self.lstm = nn.LSTM(
            input_size=hidden_size,
            hidden_size=hidden_size,
            num_layers=num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0,
            bidirectional=bidirectional
        )
        
        # Attention
        lstm_output_size = hidden_size * self.num_directions
        if use_attention:
            self.attention = Attention(lstm_output_size)
        
        # Fully connected layers
        fc_input_size = lstm_output_size
        self.fc = nn.Sequential(
            nn.Linear(fc_input_size, hidden_size),
            nn.LayerNorm(hidden_size),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(hidden_size, hidden_size // 2),
            nn.LayerNorm(hidden_size // 2),
            nn.ReLU(),
            nn.Dropout(dropout)
        )
        
        # Output layer
        if output_type == 'regression':
            self.output = nn.Linear(hidden_size // 2, 1)
        elif output_type == 'classification':
            self.output = nn.Linear(hidden_size // 2, num_classes)
        elif output_type == 'binary':
            self.output = nn.Linear(hidden_size // 2, 1)
        else:
            raise ValueError(f"Unknown output type: {output_type}")
    
    def forward(
        self, 
        x: torch.Tensor,
        return_attention: bool = False
    ) -> torch.Tensor:
        """
        Forward pass
        
        Args:
            x: Input tensor of shape (batch, seq_len, input_size)
            return_attention: Whether to return attention weights
        
        Returns:
            Output predictions
        """
        batch_size = x.size(0)
        
        # Input projection
        x = self.input_projection(x)  # (batch, seq_len, hidden_size)
        
        # LSTM
        lstm_out, (h_n, c_n) = self.lstm(x)
        # lstm_out: (batch, seq_len, hidden_size * num_directions)
        
        # Get representation
        if self.use_attention:
            context, attention_weights = self.attention(lstm_out)
        else:
            # Use last hidden state
            if self.bidirectional:
                # Concatenate forward and backward final hidden states
                h_forward = h_n[-2]  # Last layer, forward
                h_backward = h_n[-1]  # Last layer, backward
                context = torch.cat([h_forward, h_backward], dim=1)
            else:
                context = h_n[-1]
            attention_weights = None
        
        # Fully connected layers
        fc_out = self.fc(context)
        
        # Output
        output = self.output(fc_out)
        
        if self.output_type == 'binary':
            output = torch.sigmoid(output)
        
        if return_attention and self.use_attention:
            return output, attention_weights
        return output
    
    def predict(self, x: torch.Tensor) -> np.ndarray:
        """Make predictions (numpy output)"""
        self.eval()
        with torch.no_grad():
            output = self.forward(x)
            if self.output_type == 'classification':
                return torch.argmax(output, dim=1).cpu().numpy()
            return output.squeeze(-1).cpu().numpy()


class LSTMWithResidual(nn.Module):
    """
    LSTM with residual connections for better gradient flow
    """
    
    def __init__(
        self,
        input_size: int,
        hidden_size: int = 128,
        num_layers: int = 3,
        dropout: float = 0.2,
        output_type: str = 'regression'
    ):
        super().__init__()
        
        self.input_projection = nn.Linear(input_size, hidden_size)
        
        # Stack of LSTM layers with residual connections
        self.lstm_layers = nn.ModuleList()
        self.layer_norms = nn.ModuleList()
        
        for i in range(num_layers):
            self.lstm_layers.append(
                nn.LSTM(hidden_size, hidden_size, batch_first=True)
            )
            self.layer_norms.append(nn.LayerNorm(hidden_size))
        
        self.dropout = nn.Dropout(dropout)
        
        self.fc = nn.Sequential(
            nn.Linear(hidden_size, hidden_size // 2),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(hidden_size // 2, 1)
        )
        
        self.output_type = output_type
    
    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = self.input_projection(x)
        
        for lstm, ln in zip(self.lstm_layers, self.layer_norms):
            residual = x
            lstm_out, _ = lstm(x)
            x = ln(lstm_out + residual)
            x = self.dropout(x)
        
        # Use last time step
        x = x[:, -1, :]
        output = self.fc(x)
        
        return output


class TransformerTradingModel(nn.Module):
    """
    Transformer-based model as an alternative to LSTM
    May capture longer-range dependencies better
    """
    
    def __init__(
        self,
        input_size: int,
        d_model: int = 128,
        nhead: int = 8,
        num_layers: int = 3,
        dropout: float = 0.1,
        max_seq_len: int = 100,
        output_type: str = 'regression'
    ):
        super().__init__()
        
        self.input_projection = nn.Linear(input_size, d_model)
        
        # Positional encoding
        self.pos_encoding = nn.Parameter(torch.randn(1, max_seq_len, d_model) * 0.1)
        
        # Transformer encoder
        encoder_layer = nn.TransformerEncoderLayer(
            d_model=d_model,
            nhead=nhead,
            dim_feedforward=d_model * 4,
            dropout=dropout,
            batch_first=True
        )
        self.transformer = nn.TransformerEncoder(encoder_layer, num_layers=num_layers)
        
        # Output layers
        self.fc = nn.Sequential(
            nn.Linear(d_model, d_model // 2),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(d_model // 2, 1)
        )
        
        self.output_type = output_type
    
    def forward(self, x: torch.Tensor) -> torch.Tensor:
        seq_len = x.size(1)
        
        # Project input
        x = self.input_projection(x)
        
        # Add positional encoding
        x = x + self.pos_encoding[:, :seq_len, :]
        
        # Transformer
        x = self.transformer(x)
        
        # Use last position or average
        x = x[:, -1, :]  # Last position
        
        # Output
        output = self.fc(x)
        
        return output


def create_model(
    model_type: str,
    input_size: int,
    config: Optional[Dict] = None
) -> nn.Module:
    """
    Factory function to create models
    
    Args:
        model_type: 'lstm', 'lstm_residual', 'transformer'
        input_size: Number of input features
        config: Model configuration dictionary
    """
    config = config or {}
    
    if model_type == 'lstm':
        return LSTMTradingModel(
            input_size=input_size,
            hidden_size=config.get('hidden_size', 128),
            num_layers=config.get('num_layers', 2),
            dropout=config.get('dropout', 0.2),
            bidirectional=config.get('bidirectional', True),
            use_attention=config.get('use_attention', True),
            output_type=config.get('output_type', 'regression')
        )
    elif model_type == 'lstm_residual':
        return LSTMWithResidual(
            input_size=input_size,
            hidden_size=config.get('hidden_size', 128),
            num_layers=config.get('num_layers', 3),
            dropout=config.get('dropout', 0.2),
            output_type=config.get('output_type', 'regression')
        )
    elif model_type == 'transformer':
        return TransformerTradingModel(
            input_size=input_size,
            d_model=config.get('d_model', 128),
            nhead=config.get('nhead', 8),
            num_layers=config.get('num_layers', 3),
            dropout=config.get('dropout', 0.1),
            output_type=config.get('output_type', 'regression')
        )
    else:
        raise ValueError(f"Unknown model type: {model_type}")


if __name__ == "__main__":
    # Test model creation and forward pass
    batch_size = 32
    seq_len = 50
    input_size = 45
    
    # Create sample input
    x = torch.randn(batch_size, seq_len, input_size)
    
    print("Testing LSTM model...")
    model = create_model('lstm', input_size, {'hidden_size': 64, 'num_layers': 2})
    print(f"Model parameters: {sum(p.numel() for p in model.parameters()):,}")
    output = model(x)
    print(f"Output shape: {output.shape}")
    
    print("\nTesting LSTM with residual...")
    model = create_model('lstm_residual', input_size, {'hidden_size': 64, 'num_layers': 3})
    print(f"Model parameters: {sum(p.numel() for p in model.parameters()):,}")
    output = model(x)
    print(f"Output shape: {output.shape}")
    
    print("\nTesting Transformer model...")
    model = create_model('transformer', input_size, {'d_model': 64, 'num_layers': 2})
    print(f"Model parameters: {sum(p.numel() for p in model.parameters()):,}")
    output = model(x)
    print(f"Output shape: {output.shape}")
    
    print("\nAll models working correctly!")
