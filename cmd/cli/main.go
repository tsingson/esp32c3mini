package main

import (
	"fmt"
	"io"
	"log"
	"time"

	// 明确且唯一引入的第三方串口库
	"github.com/tarm/serial"
)

const (
	StreamData   byte = 0x00
	StreamEOF    byte = 0x01
	StreamCancel byte = 0x02
)

func streamWithCancelDemo(portName string) {
	config := &serial.Config{Name: portName, Baud: 115200, ReadTimeout: time.Second * 5}
	s, err := serial.OpenPort(config)
	if err != nil {
		log.Fatalf("无法打开串口: %v", err)
	}
	defer s.Close()

	fmt.Println("[Go-Client] 初始化串口管道...")
	time.Sleep(time.Millisecond * 2000)
	s.Flush()

	fmt.Println("[Go-Client] 发送流式开启命令...")
	startBuf := []byte{0xAA, 0xBB, 0x03, StreamData, 0x00}
	var sum byte
	for i := 0; i < 4; i++ {
		sum += startBuf[i]
	}
	startBuf[4] = sum
	s.Write(startBuf)

	fmt.Println("[Go-Client] 建立连接成功。进入无限流式接收状态机...")

	packetCount := 0

	for {
		rxBuf := make([]byte, 5)
		_, err := io.ReadFull(s, rxBuf)
		if err != nil {
			log.Fatalf("\n❌ 物理读取终止: %v", err)
		}

		if rxBuf[0] != 0xAA || rxBuf[1] != 0xBB {
			continue
		}
		var calcSum byte
		for i := 0; i < 4; i++ {
			calcSum += rxBuf[i]
		}
		if rxBuf[4] != calcSum {
			continue
		}

		streamStatus := rxBuf[3]
		dataVal := rxBuf[4]

		switch streamStatus {
		case StreamData:
			packetCount++
			fmt.Printf("[%s] 📥 [Streaming] 收到数据 -> Value = %d\n", time.Now().Format("15:04:05"), dataVal)

			// 🚀【核心主动中止触发点】：当收到第 3 帧数据时，Go 模拟触发主动取消机制
			if packetCount == 3 {
				fmt.Println("\n🛑 [Go-Client] 触发中途主动中止动作！向 ESP32 发送 CANCEL 信号...")

				cancelBuf := []byte{0xAA, 0xBB, 0x03, StreamCancel, 0x00}
				var cSum byte
				for i := 0; i < 4; i++ {
					cSum += cancelBuf[i]
				}
				cancelBuf[4] = cSum

				// 逆向写入取消帧
				_, err = s.Write(cancelBuf)
				if err != nil {
					fmt.Printf("发送取消信号失败: %v\n", err)
				}
			}

		case StreamCancel:
			// 🚀 收到来自 ESP32 回应的流式强制中止确认，完美闭合流
			fmt.Printf("\n🤝 [%s] ======= 收到 ESP32 的 CANCEL (STREAM_CANCEL) 确认信号 =======\n", time.Now().Format("15:04:05"))
			fmt.Println("🎉 客户端成功中途斩断流传输，通道安全闭合！")
			return

		case StreamEOF:
			fmt.Println("\n🏁 收到完整的流结束信号。")
			return
		}
	}
}

func main() {
	serialPort := "/dev/cu.usbmodem1101"
	streamWithCancelDemo(serialPort)
}
