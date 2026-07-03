package main

import (
	"fmt"
	"io"
	"log"
	"time"

	// 明确且唯一引入的第三方串口库
	"github.com/tarm/serial"
)

 // sendAndReceive 显式利用 tarm/serial 进行二进制交互
 func sendAndReceive(portName string, cmd byte, val byte) (byte, error) {
 	// 直连 tarm/serial 的配置实例
 	config := &serial.Config{
 		Name:        portName,
 		Baud:        115200,
 		ReadTimeout: time.Millisecond * 1200, // 设定 1.2 秒的安全读取超时
 	}

 	s, err := serial.OpenPort(config)
 	if err != nil {
 		return 0, fmt.Errorf("无法打开端口: %v", err)
 	}
 	defer s.Close()

 	// 🚀 【关键】给内置虚拟 USB-Serial-JTAG 端口留出驱动级总线枚举锁定的时间，避免数据滑落
 	time.Sleep(time.Millisecond * 1000)

 	// 组装 5-byte 数据帧
 	txBuf := make([]byte, 5)
 	txBuf[0] = 0xAA
 	txBuf[1] = 0xBB
 	txBuf[2] = cmd
 	txBuf[3] = val
 	txBuf[4] = txBuf[0] + txBuf[1] + cmd + val // 校验和计算

 	// 强制清空硬件缓冲区残留
 	s.Flush()

 	// 写入指令
 	_, err = s.Write(txBuf)
 	if err != nil {
 		return 0, fmt.Errorf("写入错误: %v", err)
 	}

 	// 强制使用 io.ReadFull 保证在 tarm/serial 缓冲区积攒足够 5 字节前绝不提前收流
 	rxBuf := make([]byte, 5)
 	_, err = io.ReadFull(s, rxBuf)
 	if err != nil {
 		return 0, fmt.Errorf("读取完整回执失败或触发超时断开 (EOF): %v", err)
 	}

 	// 包校验验证
 	if rxBuf[0] != 0xAA || rxBuf[1] != 0xBB {
 		return 0, fmt.Errorf("无效的回执帧头: [0x%X 0x%X]", rxBuf[0], rxBuf[1])
 	}

 	calcSum := rxBuf[0] + rxBuf[1] + rxBuf[2] + rxBuf[3]
 	if rxBuf[4] != calcSum {
 		return 0, fmt.Errorf("回执校验和错误: 期待 0x%X, 收到 0x%X", calcSum, rxBuf[4])
 	}

 	return rxBuf[3], nil
 }

 func main() {
 	// 目标虚拟串口路径
 	serialPort := "/dev/cu.usbmodem1101"

 	// 功能动作：“read”或“write”
 	action := "read"
 	writeValue := 1 // 当 action 为 write 时的改写目标值

 	switch action {
 	case "read":
 		fmt.Printf("[Go-Serial] 正在通过 tarm/serial 读取设备 %s ...\n", serialPort)
 		resVal, err := sendAndReceive(serialPort, 0x01, 0x00)
 		if err != nil {
 			log.Fatalf("❌ 操作失败: %v", err)
 		}
 		fmt.Printf("🎉 读取成功！当前 httpd_enble 值为: %d\n", resVal)

 	case "write":
 		fmt.Printf("[Go-Serial] 正在通过 tarm/serial 修改设备 %s 值为 %d ...\n", serialPort, writeValue)
 		resVal, err := sendAndReceive(serialPort, 0x02, byte(writeValue))
 		if err != nil {
 			log.Fatalf("❌ 操作失败: %v", err)
 		}
 		if resVal == 0x01 {
 			fmt.Println("🚀 成功！ESP32 已接收新参数并同步固化至其内部 NVS Flash。")
 		} else {
 			fmt.Println("❌ 失败：写入请求遭芯片底层拒绝。")
 		}
 	}
 }
