# Middleware for Kerong based lockers

This is a simple interface to connect a Kerong control board, to the Cloud via MQTT broker, the steps to install are the following:

* Before anything, please copy `lkfw.conf.example to `lkfw.conf` and edit according to your environment.

```
cd lkfw/
sudo ./install_required.sh 
sudo make all
sudo make install
```
