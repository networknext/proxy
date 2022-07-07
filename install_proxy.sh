#!/bin/bash

sudo mkdir -p /app
sudo cp -f ./bin/proxy /app/proxy
sudo cp -f proxy.env /app/proxy.env

sudo systemctl stop proxy

sudo chmod +x /app/proxy

sudo cp proxy.service /etc/systemd/system/proxy.service

sudo sysctl -w net.core.rmem_max=1000000000
sudo sysctl -w net.core.wmem_max=1000000000

sudo systemctl daemon-reload

sudo systemctl start proxy.service
