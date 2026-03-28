#!/usr/bin/env python3
"""
Simple mock microservices for testing Throttlr gateway.
Run this to simulate real backend services.
"""

from http.server import HTTPServer, BaseHTTPRequestHandler
import json
import threading
import sys

# Mock data
USERS = [
    {"id": 1, "name": "John Doe", "email": "john@example.com"},
    {"id": 2, "name": "Jane Smith", "email": "jane@example.com"},
    {"id": 3, "name": "Bob Wilson", "email": "bob@example.com"}
]

PRODUCTS = [
    {"id": 1, "name": "Laptop", "price": 999.99, "stock": 50},
    {"id": 2, "name": "Phone", "price": 699.99, "stock": 100},
    {"id": 3, "name": "Tablet", "price": 499.99, "stock": 75}
]

ORDERS = [
    {"id": 1, "user_id": 1, "product_id": 1, "quantity": 1, "status": "shipped"},
    {"id": 2, "user_id": 2, "product_id": 2, "quantity": 2, "status": "pending"}
]

class MockServiceHandler(BaseHTTPRequestHandler):
    service_name = "generic"
    data = []

    def log_message(self, format, *args):
        print(f"[{self.service_name}] {args[0]}")

    def send_json(self, data, status=200):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("X-Service", self.service_name)
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())

    def do_GET(self):
        if self.path == "/health":
            self.send_json({"status": "healthy", "service": self.service_name})
        elif self.path.startswith(f"/{self.service_name}"):
            # Get item by ID
            parts = self.path.split("/")
            if len(parts) == 3 and parts[2].isdigit():
                item_id = int(parts[2])
                item = next((x for x in self.data if x["id"] == item_id), None)
                if item:
                    self.send_json(item)
                else:
                    self.send_json({"error": "Not found"}, 404)
            else:
                self.send_json(self.data)
        else:
            self.send_json(self.data)

    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length)
        try:
            data = json.loads(body) if body else {}
            data["id"] = len(self.data) + 1
            self.data.append(data)
            self.send_json(data, 201)
        except:
            self.send_json({"error": "Invalid JSON"}, 400)

def create_handler(service_name, data):
    class Handler(MockServiceHandler):
        pass
    Handler.service_name = service_name
    Handler.data = data
    return Handler

def run_service(name, port, data):
    handler = create_handler(name, data)
    server = HTTPServer(("127.0.0.1", port), handler)
    print(f"[{name}] Running on http://127.0.0.1:{port}")
    server.serve_forever()

if __name__ == "__main__":
    services = [
        ("users", 3001, USERS),
        ("products", 3002, PRODUCTS),
        ("orders", 3003, ORDERS)
    ]

    print("Starting mock microservices...")
    print("-" * 40)

    threads = []
    for name, port, data in services:
        t = threading.Thread(target=run_service, args=(name, port, data), daemon=True)
        t.start()
        threads.append(t)

    print("-" * 40)
    print("All services running. Press Ctrl+C to stop.")
    print("\nTest with:")
    print("  curl http://localhost:8080/users")
    print("  curl http://localhost:8080/products")
    print("  curl http://localhost:8080/orders")
    print("  curl http://localhost:8080/users/1")

    try:
        while True:
            pass
    except KeyboardInterrupt:
        print("\nStopping services...")
