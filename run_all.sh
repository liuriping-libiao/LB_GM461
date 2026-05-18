#!/bin/bash
# LBGM461 目标板一键启动脚本
# 功能: 配置网络地址 169.254.0.200 (持久化) + 启动 camera_service + 启动 LBGripper

set -e

IFACE="eth0"
STATIC_IP="169.254.0.200"
NETMASK="255.255.0.0"
PREFIX="16"

echo '=== 配置网络地址 ==='
# 追加静态IP地址（不清除已有地址，避免断开SSH）
if ! ip addr show dev $IFACE | grep -q "${STATIC_IP}"; then
    sudo ip addr add ${STATIC_IP}/${PREFIX} dev $IFACE 2>/dev/null || true
fi
sudo ip link set $IFACE up

echo "已设置 $IFACE -> $STATIC_IP/$PREFIX"

# 持久化保存: 写入 /etc/network/interfaces.d/ (Debian)
IFACE_FILE="/etc/network/interfaces.d/${IFACE}-static"
sudo tee $IFACE_FILE > /dev/null <<EOF
auto ${IFACE}
iface ${IFACE} inet static
    address ${STATIC_IP}
    netmask ${NETMASK}
EOF
echo "地址已持久化保存到 $IFACE_FILE"

sleep 1

echo ''
echo '=== 启动 camera_service ==='
pkill -9 -x camera_service 2>/dev/null || true
sleep 2

cd /home/cat/camera_service
rm -f /tmp/camera_service.log
LD_LIBRARY_PATH=/home/cat/camera_service nohup ./camera_service \
  --listen 0.0.0.0:5111 --camera-ip 169.254.0.10 \
  </dev/null >/tmp/camera_service.log 2>&1 &
CS_PID=$!
echo "camera_service PID=$CS_PID"

sleep 3
if ps -p $CS_PID > /dev/null 2>&1; then
    echo 'camera_service 启动成功'
else
    echo 'camera_service 启动失败'
    tail -10 /tmp/camera_service.log
    exit 1
fi

echo ''
echo '=== 启动 LBGripper ==='
pkill -9 -f /home/cat/LBGripper/LBGripper 2>/dev/null || true
sleep 2

cd /home/cat/LBGripper
rm -f /tmp/lbgripper.log
nohup ./LBGripper </dev/null >/tmp/lbgripper.log 2>&1 &
LB_PID=$!
echo "LBGripper PID=$LB_PID"

sleep 3
if ps -p $LB_PID > /dev/null 2>&1; then
    echo 'LBGripper 启动成功'
else
    echo 'LBGripper 启动失败'
    tail -10 /tmp/lbgripper.log
    exit 1
fi

echo ''
echo '=== 全部启动完成 ==='
echo "网络地址: $STATIC_IP/$PREFIX (已持久化)"
echo "camera_service: PID=$CS_PID 日志=/tmp/camera_service.log"
echo "LBGripper: PID=$LB_PID 日志=/tmp/lbgripper.log"
