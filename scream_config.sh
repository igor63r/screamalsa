#!/bin/bash

CONF_FILE="./scream.conf"
MODULE="snd_screamalsa"

if [ ! -f "$CONF_FILE" ]; then
    echo "Configuration file $CONF_FILE not found."
    exit 1
fi

# Load variables from the configuration file
source <(grep = "$CONF_FILE" | sed 's/ *= */=/g')

# Check if the module is loaded
if ! lsmod | grep -q "$MODULE"; then
    echo "Module $MODULE is not loaded."
    exit 1
fi

# Write values to the module parameters via sysfs
echo "$IP" > "/sys/module/$MODULE/parameters/ip_addr_str"
echo "$PORT" > "/sys/module/$MODULE/parameters/port"
echo "$PROTOCOL" > "/sys/module/$MODULE/parameters/protocol_str"
echo "options snd-screamalsa ip_addr_str=$IP  port=$PORT protocol_str=$PROTOCOL" > /etc/modprobe.d/screamalsa.conf
echo "Scream configuration successfully applied:"
echo "  IP: $(cat /sys/module/$MODULE/parameters/ip_addr_str)"
echo "  Port: $(cat /sys/module/$MODULE/parameters/port)"
echo "  Protocol: $(cat /sys/module/$MODULE/parameters/protocol_str)"