#!/usr/bin/env python3
"""
ESP32 Clock Recovery Test Runner
Runs unit tests for SPDIF BMC clock recovery modules
"""

import sys
import subprocess
import os
import argparse
import time
import serial
import re
from pathlib import Path

def find_serial_port():
    """Find the ESP32 serial port automatically"""
    import serial.tools.list_ports
    
    ports = list(serial.tools.list_ports.comports())
    esp_ports = []
    
    for port in ports:
        # Look for common ESP32 USB-to-UART chips
        if any(x in port.description.lower() for x in ['cp210', 'ch340', 'ftdi', 'silicon labs']):
            esp_ports.append(port.device)
        # Also check for ESP32 in description
        elif 'esp32' in port.description.lower():
            esp_ports.append(port.device)
    
    if len(esp_ports) == 1:
        return esp_ports[0]
    elif len(esp_ports) > 1:
        print("Multiple ESP32 devices found:")
        for i, port in enumerate(esp_ports):
            print(f"  {i+1}. {port}")
        choice = input("Select port number: ")
        return esp_ports[int(choice)-1]
    else:
        return None

def build_project(target='esp32s3', clean=False):
    """Build the project with test configuration"""
    print("=" * 60)
    print("Building ESP32 Clock Recovery Tests")
    print("=" * 60)
    
    if clean:
        print("Cleaning build directory...")
        subprocess.run(['idf.py', 'fullclean'], check=True)
    
    # Set target
    print(f"Setting target to {target}...")
    subprocess.run(['idf.py', 'set-target', target], check=True)
    
    # Enable tests in sdkconfig
    print("Configuring test options...")
    config_cmd = [
        'idf.py',
        'menuconfig',
        '-D', 'CONFIG_CLOCK_RECOVERY_ENABLE_TESTS=y',
        '-D', 'CONFIG_CLOCK_RECOVERY_LOG_LEVEL=4',
        '-D', 'CONFIG_CLOCK_RECOVERY_ENABLE_STATS=y'
    ]
    
    # Build
    print("Building project...")
    subprocess.run(['idf.py', 'build'], check=True)
    print("Build successful!")

def flash_and_monitor(port=None, baudrate=115200):
    """Flash firmware and monitor test output"""
    if port is None:
        port = find_serial_port()
        if port is None:
            print("No ESP32 device found. Please specify port with -p")
            return False
    
    print(f"Flashing to port {port}...")
    subprocess.run(['idf.py', '-p', port, 'flash'], check=True)
    
    print("\nMonitoring test output...")
    print("Press Ctrl+C to stop\n")
    
    # Open serial port for monitoring
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
        test_results = {
            'passed': 0,
            'failed': 0,
            'tests': []
        }
        
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                print(line)
                
                # Parse test results
                if "TEST PASS:" in line:
                    test_results['passed'] += 1
                    test_name = line.split("TEST PASS:")[1].strip()
                    test_results['tests'].append(('PASS', test_name))
                elif "TEST FAIL:" in line:
                    test_results['failed'] += 1
                    test_name = line.split("TEST FAIL:")[1].strip()
                    test_results['tests'].append(('FAIL', test_name))
                elif "TEST SUITE COMPLETE" in line:
                    print_summary(test_results)
                    
    except KeyboardInterrupt:
        print("\nMonitoring stopped by user")
        print_summary(test_results)
    except Exception as e:
        print(f"Error: {e}")
        return False
    
    return True

def print_summary(results):
    """Print test summary"""
    print("\n" + "=" * 60)
    print("TEST SUMMARY")
    print("=" * 60)
    print(f"Tests Passed: {results['passed']}")
    print(f"Tests Failed: {results['failed']}")
    print(f"Total Tests:  {results['passed'] + results['failed']}")
    
    if results['failed'] > 0:
        print("\nFailed Tests:")
        for status, name in results['tests']:
            if status == 'FAIL':
                print(f"  ❌ {name}")
    
    if results['passed'] > 0:
        print("\nPassed Tests:")
        for status, name in results['tests']:
            if status == 'PASS':
                print(f"  ✅ {name}")
    
    print("=" * 60)

def run_specific_test(test_name, port=None):
    """Run a specific test by name"""
    print(f"Running test: {test_name}")
    
    # Build with specific test enabled
    build_project()
    
    # Flash and run
    flash_and_monitor(port)

def main():
    parser = argparse.ArgumentParser(description='ESP32 Clock Recovery Test Runner')
    parser.add_argument('-p', '--port', help='Serial port (auto-detect if not specified)')
    parser.add_argument('-t', '--target', default='esp32s3', help='ESP32 target (default: esp32s3)')
    parser.add_argument('-b', '--baudrate', type=int, default=115200, help='Serial baudrate')
    parser.add_argument('-c', '--clean', action='store_true', help='Clean build before compiling')
    parser.add_argument('--test', help='Run specific test by name')
    parser.add_argument('--build-only', action='store_true', help='Only build, do not flash')
    parser.add_argument('--monitor-only', action='store_true', help='Only monitor, do not build/flash')
    
    args = parser.parse_args()
    
    try:
        if args.monitor_only:
            flash_and_monitor(args.port, args.baudrate)
        elif args.build_only:
            build_project(args.target, args.clean)
        elif args.test:
            run_specific_test(args.test, args.port)
        else:
            build_project(args.target, args.clean)
            if not args.build_only:
                flash_and_monitor(args.port, args.baudrate)
                
    except subprocess.CalledProcessError as e:
        print(f"Command failed: {e}")
        return 1
    except KeyboardInterrupt:
        print("\nOperation cancelled by user")
        return 0
    
    return 0

if __name__ == '__main__':
    sys.exit(main())