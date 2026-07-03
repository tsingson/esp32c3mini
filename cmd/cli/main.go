package main

import (
	"fmt"
	"log"
	"time"

	// 明确且唯一引入的第三方串口库
	"github.com/tarm/serial"
)

const (
	CmdBidiStream byte = 0x05
	StreamData    byte = 0x00
	StreamEOF     byte = 0x01
	StreamCancel  byte = 0x02
)

func sendPacket(s *serial.Port, cmd, status, val byte) error {
	txBuf := []byte{0xAA, 0xBB, cmd, status, val}
	var sum byte
	for i := 0; i < 4; i++ {
		sum += txBuf[i]
	}
	txBuf[4] = sum
	_, err := s.Write(txBuf)
	return err
}

func startRobustBidiStreaming(portName string) {
	config := &serial.Config{Name: portName, Baud: 115200, ReadTimeout: time.Millisecond * 2000}
	s, err := serial.OpenPort(config)
	if err != nil {
		log.Fatalf("无法打开串口: %v", err)
	}
	defer s.Close()

	fmt.Println("[Go-Client] 初始化工业级全双工串口总线...")
	time.Sleep(time.Millisecond * 2000)
	s.Flush()

	bidiCtxDone := make(chan bool, 1)

	// ------------------------------------------------------------------
	// 协程分流 1：升级为【滑动窗口单字节状态机】的异步接收回路
	// ------------------------------------------------------------------
	go func() {
		fmt.Println("[Go-StreamReader] 自愈状态机已拉起，开始监控上行流...")

		var rawBuf [1]byte
		packetBuf := make([]byte, 5)
		stateIdx := 0

		for {
			// 单字节读取，天然对齐流式传输
			n, err := s.Read(rawBuf[:])
			if err != nil {
				fmt.Printf("\n[Go-StreamReader] 管道物理中断退出: %v\n", err)
				bidiCtxDone <- true
				return
			}
			if n == 0 {
				continue
			}

			ch := rawBuf[0]

			// 防御性边界越界锁
			if stateIdx >= 5 {
				stateIdx = 0
			}
			packetBuf[stateIdx] = ch
			stateIdx++

			// 🚀 自愈核心：动态扫描对齐帧头 🚀
			if stateIdx == 1 && packetBuf[0] != 0xAA {
				stateIdx = 0
				continue
			}
			if stateIdx == 2 && packetBuf[1] != 0xBB {
				if packetBuf[1] == 0xAA { // 容错 0xAA 0xAA 0xBB 的情况
					packetBuf[0] = 0xAA
					stateIdx = 1
				} else {
					stateIdx = 0
				}
				continue
			}

			// 成功攒满 5 字节无错包
			if stateIdx == 5 {
				cmd := packetBuf[2]
				streamFlag := packetBuf[3]
				serverStatusVal := packetBuf[4]

				// 重置索引给下一包准备
				stateIdx = 0

				// 过滤非本项目命令
				if cmd != CmdBidiStream {
					continue
				}

				// 校验和验证
				var calcSum byte
				for i := 0; i < 4; i++ {
					calcSum += packetBuf[i]
				}
				if serverStatusVal != calcSum {
					fmt.Println("⚠️  [Go-StreamReader] 抓到一条数据校验和错误包，已自动剔除自愈。")
					continue
				}

				// 协议业务分发
				switch streamFlag {
				case StreamData:
					fmt.Printf("           📈 [ESP32 反馈流] 物理逼近状态 -> 实时值 = %d\n", packetBuf[3]) // 修正：按照 C 端格式，反馈流 status 位携带真实演变值
				case StreamEOF:
					fmt.Println("\n🏁 [Go-StreamReader] ======= 收到对等端明确的 CLOSE STREAM 信标 =======")
					bidiCtxDone <- true
					return
				}
			}
		}
	}()

	// ------------------------------------------------------------------
	// 业务回路 2：主线程持续下行调参流 (客户端 -> 服务端)
	// ------------------------------------------------------------------
	fmt.Println("[Go-Client] 🚀 双向流就绪，开始动态注入参数进行压力测试...")

	dynamicParams := []byte{40, 5, 80, 0}

	for _, nextTarget := range dynamicParams {
		fmt.Printf("\n⚙️  [Go-Client 动态调参] >>> 持续下发新目标参数 = %d\n", nextTarget)

		err := sendPacket(s, CmdBidiStream, StreamData, nextTarget)
		if err != nil {
			fmt.Printf("下发配置失败: %v\n", err)
			break
		}
		// 维持调参参数 1.5 秒
		time.Sleep(time.Millisecond * 1500)
	}

	fmt.Println("\n🛑 [Go-Client] 业务调整完毕，下发对等结束信标...")
	sendPacket(s, CmdBidiStream, StreamEOF, 0x00)

	// 阻塞等待异步流闭合
	<-bidiCtxDone
	fmt.Println("🎉 稳定性优化版双向流成功闭合，系统清理安全退出。")
}

func main() {
	serialPort := "/dev/cu.usbmodem1101"
	startRobustBidiStreaming(serialPort)
}
