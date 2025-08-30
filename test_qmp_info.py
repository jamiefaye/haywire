#!/usr/bin/env python3

import socket
import json

def send_qmp_command(sock, command):
    """Send QMP command and get response"""
    cmd_str = json.dumps(command) + '\n'
    sock.send(cmd_str.encode())
    response = sock.recv(65536).decode()
    try:
        return json.loads(response)
    except:
        return response

def test_qmp_queries():
    # Connect to QMP
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('localhost', 4445))
    
    # Get greeting
    greeting = sock.recv(4096).decode()
    print("Greeting:", greeting)
    
    # Negotiate capabilities
    send_qmp_command(sock, {"execute": "qmp_capabilities"})
    
    # Try different info commands
    queries = [
        {"execute": "human-monitor-command", "arguments": {"command-line": "info mtree -f"}},  # Full tree
        {"execute": "human-monitor-command", "arguments": {"command-line": "info mtree -d"}},  # With dispatch info
        {"execute": "human-monitor-command", "arguments": {"command-line": "info ramblock"}},
        {"execute": "human-monitor-command", "arguments": {"command-line": "info memory_size_summary"}},
        {"execute": "query-memory-size-summary"},
        {"execute": "human-monitor-command", "arguments": {"command-line": "info registers"}},
    ]
    
    for query in queries:
        print(f"\n{'='*60}")
        print(f"Query: {query['execute']}")
        if 'arguments' in query and 'command-line' in query['arguments']:
            print(f"  Command: {query['arguments']['command-line']}")
        print('-'*60)
        
        response = send_qmp_command(sock, query)
        
        if isinstance(response, dict):
            if 'return' in response:
                if isinstance(response['return'], str):
                    # Human monitor output
                    print(response['return'][:2000])  # Limit output
                else:
                    print(json.dumps(response['return'], indent=2))
            elif 'error' in response:
                print(f"Error: {response['error']}")
        else:
            print(response)
    
    sock.close()

if __name__ == "__main__":
    test_qmp_queries()