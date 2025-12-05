#!/usr/bin/env python3
"""
ZMQ to WebSocket Bridge for Dashboard
======================================
Bridges ZMQ metrics from the C++ backend to WebSocket for browser dashboard.

Usage:
    pip install pyzmq websockets
    python3 zmq_websocket_bridge.py

This runs on the COLD PATH - no latency concerns.
"""

import asyncio
import json
import zmq
import zmq.asyncio
from websockets.server import serve
import logging

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(message)s')
logger = logging.getLogger(__name__)

# Configuration
ZMQ_ENDPOINT = "tcp://localhost:5555"
WEBSOCKET_PORT = 8765

# Connected WebSocket clients
clients = set()

async def zmq_subscriber():
    """Subscribe to ZMQ and forward to WebSocket clients."""
    context = zmq.asyncio.Context()
    socket = context.socket(zmq.SUB)
    socket.connect(ZMQ_ENDPOINT)
    socket.setsockopt_string(zmq.SUBSCRIBE, "METRICS")
    socket.setsockopt_string(zmq.SUBSCRIBE, "EQUITY")
    socket.setsockopt_string(zmq.SUBSCRIBE, "TRADE")
    socket.setsockopt_string(zmq.SUBSCRIBE, "ALERT")
    
    logger.info(f"Connected to ZMQ at {ZMQ_ENDPOINT}")
    
    while True:
        try:
            # Receive multipart message [topic, payload]
            message = await socket.recv_multipart()
            topic = message[0].decode('utf-8')
            payload = message[1].decode('utf-8')
            
            # Forward to all WebSocket clients
            if clients:
                websocket_message = json.dumps({
                    "topic": topic,
                    "data": json.loads(payload)
                })
                await asyncio.gather(
                    *[client.send(websocket_message) for client in clients],
                    return_exceptions=True
                )
                logger.debug(f"Forwarded {topic} to {len(clients)} clients")
        except Exception as e:
            logger.error(f"ZMQ error: {e}")
            await asyncio.sleep(1)

async def websocket_handler(websocket):
    """Handle WebSocket connections from dashboard."""
    clients.add(websocket)
    client_addr = websocket.remote_address
    logger.info(f"Dashboard connected: {client_addr}")
    
    try:
        async for message in websocket:
            # Handle any commands from dashboard (e.g., subscribe to specific symbols)
            try:
                cmd = json.loads(message)
                logger.info(f"Received command: {cmd}")
            except:
                pass
    except Exception as e:
        logger.info(f"Dashboard disconnected: {client_addr}")
    finally:
        clients.remove(websocket)

async def main():
    """Main entry point."""
    print("""
╔══════════════════════════════════════════════════════════════════════════════╗
║               ZMQ → WEBSOCKET BRIDGE FOR DASHBOARD                           ║
╠══════════════════════════════════════════════════════════════════════════════╣
║ ZMQ Subscribe:     tcp://localhost:5555                                      ║
║ WebSocket Server:  ws://localhost:8765                                       ║
╠══════════════════════════════════════════════════════════════════════════════╣
║ To use:                                                                      ║
║   1. Start the C++ dashboard server (publishes to ZMQ)                       ║
║   2. Start this bridge (python3 zmq_websocket_bridge.py)                     ║
║   3. Open dashboard/strategy_dashboard.html in browser                       ║
╚══════════════════════════════════════════════════════════════════════════════╝
    """)
    
    # Start WebSocket server
    async with serve(websocket_handler, "localhost", WEBSOCKET_PORT):
        logger.info(f"WebSocket server running on ws://localhost:{WEBSOCKET_PORT}")
        
        # Start ZMQ subscriber
        await zmq_subscriber()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Shutting down...")
