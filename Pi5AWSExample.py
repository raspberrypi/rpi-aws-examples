# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (C) 2025 Raspberry Pi Ltd
#
# Pi5AWSExample.py

# This example code is a very basic Python application to pass the 
# AWS Device Advisor qualification testing

import sys
import time
import random
from enum import Enum

from awscrt import io, mqtt, auth, http
from awsiot import mqtt_connection_builder

# Configuration - replace these with your actual file paths / names
THING_NAME = "Pi5AWSExample"
CLIENT_ID = THING_NAME
IOT_ENDPOINT = "t2zl79cjjnlkx1.deviceadvisor.iot.us-east-1.amazonaws.com"
ROOT_CA_PATH = "AmazonRootCA1.pem"
PRIVATE_KEY_PATH = "03c89d548efc444d327b49514ee9e647ea8a7e23d0fc25fc762cxxxxxxxxxxxx-private.pem.key"
CERTIFICATE_PATH = "03c89d548efc444d327b49514ee9e647ea8a7e23d0fc25fc762cxxxxxxxxxxxx-certificate.pem.crt"

# Topics used for simple publish/subscribe test
TEST_PUB_TOPIC = f"Pi5AWSExample_pub"
TEST_SUB_TOPIC = f"Pi5AWSExample_sub"

MAX_RETRIES=30

# Event Handlers/Callbacks. Useful for debugging
def on_connection_interrupted(connection, error, **kwargs):
    print(f"[!] Connection interrupted: {error}")

def on_connection_resumed(connection, return_code, session_present, **kwargs):
    print("[+] Connection resumed")

def on_message_received(topic, payload, dup, qos, retain, **kwargs):
    print(f"[OK] Received message: {payload.decode()} from topic: {topic}")

def on_connection_success(connection, callback_data):
    print(callback_data)

# Helpers
def Connect(mqtt_connection, retries):
    print("Connecting...")
    connect_future = mqtt_connection.connect()
    connect_future.result()
    print("Connected!")
    return True
        
def Connect_with_exponential_backoff(mqtt_connection, retries):
    attempt = 0
    while True:
        try:
            print("Attempting MQTT connect...")
            connect = mqtt_connection.connect()
            connect.result()
            print("Connected to AWS IoT Core.")
            return True
        except Exception as e:
            backoff = min(2 ** attempt, 32)
            jitter = random.uniform(0, 1)
            sleep_time = backoff + jitter
            print(f"Connection failed: {e}. Sleeping {sleep_time:.2f}s before retry.")
            time.sleep(sleep_time)
            attempt += 1
            if (attempt == retries): 
                return False

# Run for specified time, occasionally doing a publish to keep the connection active
def Dummy_workload(mqtt_connection, runtime):
    
    endtime = time.time() + runtime
    
    while time.time() < endtime:
        Publish(mqtt_connection, 0)
        time.sleep(5)
    
def Publish(mqtt_connection, i):
    topic = TEST_PUB_TOPIC
    message = f"Hello from Pi5AWSExample {i}"
    print(f"Publishing message: '{message}' to topic '{topic}'")
    res = mqtt_connection.publish(
        topic=topic,
        payload=message,
        qos=mqtt.QoS.AT_LEAST_ONCE
    )
    print(f"Published {res}")

def Subscribe(mqtt_connection):
    topic = TEST_SUB_TOPIC
    print(f"Subscribing to topic '{topic}'...")
    subscribe_future, packet_id = mqtt_connection.subscribe(
        topic=topic,
        qos=mqtt.QoS.AT_LEAST_ONCE,
        callback=on_message_received
    )
    subscribe_result = subscribe_future.result()
    print(f"Subscribed")

def Disconnect(mqtt_connection):
    print("Disconnecting...")
    disconnect_future = mqtt_connection.disconnect()
    disconnect_future.result()
    print("Disconnected")



# MQTT Setup
event_loop_group = io.EventLoopGroup(1)
host_resolver = io.DefaultHostResolver(event_loop_group)
client_bootstrap = io.ClientBootstrap(event_loop_group, host_resolver)

mqtt_connection = mqtt_connection_builder.mtls_from_path(
    endpoint=IOT_ENDPOINT,
    port=8883,
    cert_filepath=CERTIFICATE_PATH,
    pri_key_filepath=PRIVATE_KEY_PATH,
    client_bootstrap=client_bootstrap,
    ca_filepath=ROOT_CA_PATH,
    client_id=CLIENT_ID,
    clean_session=False,
    keep_alive_secs=30,
    on_connection_interrupted=on_connection_interrupted,
    on_connection_resumed=on_connection_resumed,
    on_connection_success=on_connection_success
)

# Run forever and do various things whilst running to pass the DA tests

class state(Enum):
    CONNECT = 1
    PUBLISH = 2
    SUBSCRIBE = 3
    
state = state.CONNECT

while True:
    
    match state:
        case state.CONNECT:
            print("CONNECT")
            Connect_with_exponential_backoff(mqtt_connection, MAX_RETRIES)
            
            Dummy_workload(mqtt_connection, 15)

            Disconnect(mqtt_connection)
            
            state = state.SUBSCRIBE
        
        case state.PUBLISH:
            print("PUBLISH")
            Connect_with_exponential_backoff(mqtt_connection, MAX_RETRIES)
            
            for i in range(0,5):
                time.sleep(1)
                Publish(mqtt_connection, i)
                
            time.sleep(10)
            
            Disconnect(mqtt_connection)
            
            # Go back to start
            state = state.CONNECT
            
        case state.SUBSCRIBE:
            print("SUBSCRIBE")
            Connect_with_exponential_backoff(mqtt_connection, MAX_RETRIES)
            Subscribe(mqtt_connection)
            time.sleep(5)
            Disconnect(mqtt_connection)
            
            state = state.PUBLISH
                        
        case _:
            print("Unknown state")    
            
