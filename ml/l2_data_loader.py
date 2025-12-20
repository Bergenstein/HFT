"""
L2 Order Book Data Loader
Parses Coinbase L2 NDJSON data files and reconstructs order books
"""

import json
import os
import re
import glob
from dataclasses import dataclass, field
from typing import Dict, List, Tuple, Optional, Iterator
from collections import defaultdict
from datetime import datetime
import numpy as np
import pandas as pd
from sortedcontainers import SortedDict


def parse_timestamp(ts_str: str) -> datetime:
    """Parse ISO timestamp with nanosecond precision"""
    # Remove 'Z' and replace with +00:00
    ts_str = ts_str.replace('Z', '+00:00')
    # Truncate nanoseconds to microseconds (6 decimal places)
    # Match pattern: digits.digits+offset
    match = re.match(r'(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})\.(\d+)(\+.*)', ts_str)
    if match:
        base, frac, offset = match.groups()
        frac = frac[:6].ljust(6, '0')  # Truncate or pad to 6 digits
        ts_str = f"{base}.{frac}{offset}"
    return datetime.fromisoformat(ts_str)


@dataclass
class OrderBook:
    """Maintains a limit order book with bids and asks"""
    bids: SortedDict = field(default_factory=lambda: SortedDict(lambda x: -x))  # Price -> Quantity (descending)
    asks: SortedDict = field(default_factory=lambda: SortedDict())  # Price -> Quantity (ascending)
    
    def clear(self):
        self.bids.clear()
        self.asks.clear()
    
    def update(self, side: str, price: float, quantity: float):
        """Update a price level"""
        book = self.bids if side == 'bid' else self.asks
        if quantity == 0:
            if price in book:
                del book[price]
        else:
            book[price] = quantity
    
    def set_level(self, side: str, price: float, quantity: float):
        """Set a price level (for snapshots)"""
        self.update(side, price, quantity)
    
    @property
    def best_bid(self) -> Optional[float]:
        return self.bids.peekitem(0)[0] if self.bids else None
    
    @property
    def best_ask(self) -> Optional[float]:
        return self.asks.peekitem(0)[0] if self.asks else None
    
    @property
    def best_bid_qty(self) -> Optional[float]:
        return self.bids.peekitem(0)[1] if self.bids else None
    
    @property
    def best_ask_qty(self) -> Optional[float]:
        return self.asks.peekitem(0)[1] if self.asks else None
    
    @property
    def mid_price(self) -> Optional[float]:
        if self.best_bid and self.best_ask:
            return (self.best_bid + self.best_ask) / 2
        return None
    
    @property
    def spread(self) -> Optional[float]:
        if self.best_bid and self.best_ask:
            return self.best_ask - self.best_bid
        return None
    
    @property
    def spread_bps(self) -> Optional[float]:
        """Spread in basis points"""
        if self.mid_price and self.spread:
            return (self.spread / self.mid_price) * 10000
        return None
    
    def microprice(self) -> Optional[float]:
        """Volume-weighted microprice"""
        if self.best_bid and self.best_ask and self.best_bid_qty and self.best_ask_qty:
            total_qty = self.best_bid_qty + self.best_ask_qty
            return (self.best_bid * self.best_ask_qty + self.best_ask * self.best_bid_qty) / total_qty
        return None
    
    def top_imbalance(self) -> Optional[float]:
        """Imbalance at top of book: (bid_qty - ask_qty) / (bid_qty + ask_qty)"""
        if self.best_bid_qty and self.best_ask_qty:
            return (self.best_bid_qty - self.best_ask_qty) / (self.best_bid_qty + self.best_ask_qty)
        return None
    
    def get_depth(self, levels: int = 5) -> Tuple[List[Tuple[float, float]], List[Tuple[float, float]]]:
        """Get top N levels from both sides"""
        bid_levels = list(self.bids.items())[:levels]
        ask_levels = list(self.asks.items())[:levels]
        return bid_levels, ask_levels
    
    def depth_imbalance(self, levels: int = 5) -> Optional[float]:
        """Imbalance across top N levels"""
        bid_levels, ask_levels = self.get_depth(levels)
        bid_qty = sum(qty for _, qty in bid_levels)
        ask_qty = sum(qty for _, qty in ask_levels)
        if bid_qty + ask_qty > 0:
            return (bid_qty - ask_qty) / (bid_qty + ask_qty)
        return None
    
    def volume_weighted_price(self, side: str, levels: int = 5) -> Optional[float]:
        """Volume-weighted average price for top N levels"""
        book = self.bids if side == 'bid' else self.asks
        levels_data = list(book.items())[:levels]
        if not levels_data:
            return None
        total_qty = sum(qty for _, qty in levels_data)
        if total_qty == 0:
            return None
        return sum(price * qty for price, qty in levels_data) / total_qty
    
    def total_depth(self, side: str, levels: int = 5) -> float:
        """Total quantity across top N levels"""
        book = self.bids if side == 'bid' else self.asks
        return sum(qty for _, qty in list(book.items())[:levels])


@dataclass
class L2Update:
    """Single L2 update event"""
    timestamp: datetime
    product_id: str
    event_type: str  # 'snapshot' or 'update'
    updates: List[Dict]  # List of {side, price, quantity}
    sequence_num: int
    recv_ts_ns: int


class L2DataLoader:
    """
    Loads and processes Coinbase L2 order book data from NDJSON files
    """
    
    def __init__(self, data_dir: str):
        self.data_dir = data_dir
        self.order_books: Dict[str, OrderBook] = defaultdict(OrderBook)
        
    def get_data_files(self, pattern: str = "raw_*.ndjson") -> List[str]:
        """Get all data files matching pattern"""
        files = glob.glob(os.path.join(self.data_dir, pattern))
        return sorted(files)
    
    def parse_ndjson_line(self, line: str) -> List[L2Update]:
        """Parse a single NDJSON line - may contain multiple events"""
        results = []
        try:
            data = json.loads(line.strip())
            raw = json.loads(data['raw'])
            recv_ts_ns = data.get('ts_recv_ns', 0)
            
            if raw.get('channel') != 'l2_data':
                return results
            
            timestamp = parse_timestamp(raw['timestamp'])
            sequence_num = raw.get('sequence_num', 0)
            
            for event in raw.get('events', []):
                product_id = event.get('product_id', '')
                event_type = event.get('type', 'update')
                
                updates = []
                for update in event.get('updates', []):
                    side = update.get('side', '')
                    price = float(update.get('price_level', 0))
                    quantity = float(update.get('new_quantity', 0))
                    updates.append({
                        'side': side,
                        'price': price,
                        'quantity': quantity
                    })
                
                results.append(L2Update(
                    timestamp=timestamp,
                    product_id=product_id,
                    event_type=event_type,
                    updates=updates,
                    sequence_num=sequence_num,
                    recv_ts_ns=recv_ts_ns
                ))
        except Exception as e:
            pass
        
        return results
    
    def iter_updates(self, files: Optional[List[str]] = None) -> Iterator[L2Update]:
        """Iterate through all L2 updates from files"""
        if files is None:
            files = self.get_data_files()
        
        for filepath in files:
            with open(filepath, 'r') as f:
                for line in f:
                    updates = self.parse_ndjson_line(line)
                    for update in updates:
                        yield update
    
    def apply_update(self, update: L2Update) -> OrderBook:
        """Apply an L2 update to the order book"""
        book = self.order_books[update.product_id]
        
        if update.event_type == 'snapshot':
            book.clear()
        
        for u in update.updates:
            book.update(u['side'], u['price'], u['quantity'])
        
        return book
    
    def load_and_process(
        self,
        files: Optional[List[str]] = None,
        products: Optional[List[str]] = None,
        max_updates: Optional[int] = None
    ) -> Dict[str, List[Dict]]:
        """
        Load data and create time series of order book states
        
        Returns:
            Dict mapping product_id to list of order book snapshots
        """
        results: Dict[str, List[Dict]] = defaultdict(list)
        
        count = 0
        for update in self.iter_updates(files):
            if products and update.product_id not in products:
                continue
            
            book = self.apply_update(update)
            
            # Only record if we have a valid book
            if book.best_bid and book.best_ask:
                snapshot = {
                    'timestamp': update.timestamp,
                    'product_id': update.product_id,
                    'best_bid': book.best_bid,
                    'best_ask': book.best_ask,
                    'best_bid_qty': book.best_bid_qty,
                    'best_ask_qty': book.best_ask_qty,
                    'mid_price': book.mid_price,
                    'spread': book.spread,
                    'spread_bps': book.spread_bps,
                    'microprice': book.microprice(),
                    'top_imbalance': book.top_imbalance(),
                    'depth_imbalance_5': book.depth_imbalance(5),
                    'bid_depth_5': book.total_depth('bid', 5),
                    'ask_depth_5': book.total_depth('ask', 5),
                    'bid_vwap_5': book.volume_weighted_price('bid', 5),
                    'ask_vwap_5': book.volume_weighted_price('ask', 5),
                    'sequence_num': update.sequence_num,
                }
                results[update.product_id].append(snapshot)
            
            count += 1
            if max_updates and count >= max_updates:
                break
        
        return results
    
    def to_dataframes(
        self,
        data: Dict[str, List[Dict]]
    ) -> Dict[str, pd.DataFrame]:
        """Convert processed data to DataFrames"""
        dfs = {}
        for product_id, snapshots in data.items():
            if snapshots:
                df = pd.DataFrame(snapshots)
                df['timestamp'] = pd.to_datetime(df['timestamp'])
                df = df.set_index('timestamp')
                df = df.sort_index()
                dfs[product_id] = df
        return dfs


def discover_products(data_dir: str) -> Dict[str, int]:
    """Discover all products in the data files and their update counts"""
    loader = L2DataLoader(data_dir)
    product_counts = defaultdict(int)
    
    for update in loader.iter_updates():
        product_counts[update.product_id] += 1
    
    return dict(sorted(product_counts.items(), key=lambda x: -x[1]))


if __name__ == "__main__":
    # Test the loader
    data_dir = "/Users/israelbergenstein/Desktop/HFT_Full_Pipeline/data"
    
    print("Discovering products in data...")
    products = discover_products(data_dir)
    print(f"\nFound {len(products)} products:")
    for product, count in list(products.items())[:10]:
        print(f"  {product}: {count} updates")
    
    # Load data for most active product
    if products:
        top_product = list(products.keys())[0]
        print(f"\nLoading data for {top_product}...")
        
        loader = L2DataLoader(data_dir)
        data = loader.load_and_process(products=[top_product], max_updates=10000)
        dfs = loader.to_dataframes(data)
        
        if top_product in dfs:
            df = dfs[top_product]
            print(f"\nDataFrame shape: {df.shape}")
            print(f"\nSample data:")
            print(df.head())
            print(f"\nStatistics:")
            print(df.describe())
