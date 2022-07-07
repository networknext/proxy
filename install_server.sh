#!/bin/bash

sudo mkdir -p /app
sudo cp -f ./bin/proxy /app/server
sudo cp -f server.env /app/server.env

sudo systemctl stop server

sudo chmod +x /app/server

sudo cp server.service /etc/systemd/system/server.service

sudo systemctl daemon-reload

sudo systemctl start server.service
