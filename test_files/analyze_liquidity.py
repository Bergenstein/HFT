#!/usr/bin/env python3
"""
Quick liquidity analysis of captured L2 data
"""
import json
import sys
from collections import defaultdict

def analyze_file(filename):
    products = defaultdict(lambda: {
        'updates': 0,
        'bid_levels': [],
        'ask_levels': [],
        'spreads': []
    })
    
    with open(filename) as f:
        for line in f:
            try:
                data = json.loads(line.strip())
                if data.get('type') == 'l2update':
                    pid = data.get('product_id')
                    if pid:
                        products[pid]['updates'] += 1
                        
                        # Track changes
                        changes = data.get('changes', [])
                        for change in changes:
                            side, price, size = change
                            price_f = float(price)
                            size_f = float(size)
                            
                            if side == 'buy':
                                products[pid]['bid_levels'].append((price_f, size_f))
                            else:
                                products[pid]['ask_levels'].append((price_f, size_f))
                                
            except (json.JSONDecodeError, ValueError):
                continue
    
    # Calculate metrics
    print(f"\n=== Liquidity Analysis: {filename} ===")
    sorted_products = sorted(products.items(), key=lambda x: x[1]['updates'], reverse=True)
    
    print(f"\nTop 10 Most Active Products:")
    print(f"{'Product':<15} {'Updates':<10} {'Bid Levels':<12} {'Ask Levels':<12}")
    print("-" * 60)
    
    for pid, data in sorted_products[:10]:
        print(f"{pid:<15} {data['updates']:<10} {len(data['bid_levels']):<12} {len(data['ask_levels']):<12}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_liquidity.py <ndjson_file>")
        sys.exit(1)
    
    analyze_file(sys.argv[1])
