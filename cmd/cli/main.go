package main

import (
	"fmt"
	"io"
	"log"
	"time"

	// 明确且唯一引入的第三方串口库
	"github.com/tarm/serial"
)



 // 发送单包命令并读取响应
 func sendPacket(s *serial.Port, cmd, statusOrIdx, val byte) ([]byte, error) {
 	txBuf := make([]byte, 5)
 	txBuf[0] = 0xAA
 	txBuf[1] = 0xBB
 	txBuf[2] = cmd
 	txBuf[3] = statusOrIdx
 	txBuf[4] = val

 	var sum byte
 	for i := 0; i < 4; i++ {
 		sum += txBuf[i]
 	}
 	txBuf[4] = sum

 	s.Flush()
 	_, err := s.Write(txBuf)
 	if err != nil {
 		return nil, err
 	}

 	// 阻塞读取 5 字节回执
 	rxBuf := make([]byte, 5)
 	_, err = io.ReadFull(s, rxBuf)
 	if err != nil {
 		return nil, err
 	}

 	// 基础校验
 	if rxBuf[0] != 0xAA || rxBuf[1] != 0xBB {
 		return nil, fmt.Errorf("无效帧头")
 	}
 	var calcSum byte
 	for i := 0; i < 4; i++ {
 		calcSum += rxBuf[i]
 	}
 	if rxBuf[4] != calcSum {
 		return nil, fmt.Errorf("校验和错误")
 	}

 	return rxBuf, nil
 }



 func streamRead(portName string) {
 	// ⏳ ReadTimeout 增加到 5 秒，防止 3 秒间隔时引发超时误判
 	config := &serial.Config{Name: portName, Baud: 115200, ReadTimeout: time.Second * 5}
 	s, err := serial.OpenPort(config)
 	if err != nil {
 		log.Fatalf("无法打开串口: %v", err)
 	}
 	defer s.Close()

 	// 打开串口后，先睡眠 2.5 秒，彻底震荡掉拔插和打开串口时驱动产生的垃圾字符
 	fmt.Println("[Go-Client] 正在等待串口总线稳定...")
 	time.Sleep(time.Millisecond * 2500)

 	// 强行清空可能残留的开机杂散数据
 	s.Flush()

 	fmt.Println("[Go-Client] 发送流式读取激活指令...")
 	txBuf := []byte{0xAA, 0xBB, 0x03, 0x00, 0x00}
 	var sum byte
 	for i := 0; i < 4; i++ { sum += txBuf[i] }
 	txBuf[4] = sum
 	s.Write(txBuf)

 	fmt.Println("[Go-Client] 进入 Streaming 接收状态，等待数据喷吐...")

 	for i := 0; i < 16; i++ {
 		rxBuf := make([]byte, 5)
 		_, err := io.ReadFull(s, rxBuf)
 		if err != nil {
 			log.Fatalf("\n❌ 流式读取中断: %v", err)
 		}

 		idx := rxBuf[3]
 		val := rxBuf[4]
 		// 打印当前时间，以便肉眼观察是不是精准的 3 秒打印一次
 		fmt.Printf("[%s] ⏱️ [3.0s 步进] 收到队列数据 -> 索引位置 [%02d]: 数值 = %d\n", time.Now().Format("15:04:05"), idx, val)
 	}
 	fmt.Println("🎉 16位队列流式读取完整结束！")
 }


 // 流式写入模式
 func streamWrite(portName string) {
 	config := &serial.Config{Name: portName, Baud: 115200, ReadTimeout: time.Second * 1}
 	s, err := serial.OpenPort(config)
 	if err != nil {
 		log.Fatalf("无法打开串口: %v", err)
 	}
 	defer s.Close()

 	time.Sleep(time.Millisecond * 1000)
 	fmt.Println("[Go-Client] 开始向模拟队列流式同步写入 16 个新数值...")

 	// 模拟要写入的 16 个新数据：100 到 115
 	newData := []byte{100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115}

 	for idx, val := range newData {
 		// 每隔 0.5 秒向下级推送一包
 		fmt.Printf("📤 [0.5s 步进] 正在上报 -> 索引位置 [%02d] : 改写值 = %d\n", idx, val)

 		rx, err := sendPacket(s, 0x04, byte(idx), val)
 		if err != nil {
 			log.Fatalf("❌ 流式写入失败: %v", err)
 		}

 		if rx[4] != 0x01 {
 			fmt.Printf("⚠️ ESP32 拒绝写入索引 %d\n", idx)
 		}

 		time.Sleep(time.Second / 2) // 客户端控制 0.5 秒的上报步进
 	}
 	fmt.Println("🚀 16位队列流式覆写同步成功！")
 }

 func main() {
 	serialPort := "/dev/cu.usbmodem1101"

 	// ==========================================
 	// 模式切换控制
 	// ==========================================
 	mode := "read" // 设为 "read" 测试流式读取， 设为 "write" 测试流式写入
 	// ==========================================

 	if mode == "read" {
 		streamRead(serialPort)
 	} else {
 		streamWrite(serialPort)
 	}
 }
