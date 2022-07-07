#!/bin/bash

sudo mkdir -p /app
sudo cp -f ./bin/proxy /app/proxy
sudo cp -f proxy.env /app/proxy.env

sudo systemctl stop proxy

sudo chmod +x /app/proxy

sudo cp proxy.service /etc/systemd/system/proxy.service

sudo systemctl daemon-reload

sudo systemctl start proxy

sudo systemctl install proxy
