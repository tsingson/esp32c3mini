package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"strings"
	"time"

	// 明确且唯一引入的第三方串口库
	"github.com/tarm/serial"
)

const (
	CmdReadSSID  byte = 0x10
	CmdWriteSSID byte = 0x11
	CmdWritePass byte = 0x12
)

// 发送单字节流包
func sendStreamByte(s *serial.Port, cmd, charByte byte) error {
	txBuf := []byte{0xAA, 0xBB, cmd, charByte, 0x00}

	// 正确的 Go 语言单字节校验和累加计算
	var sum byte
	for i := 0; i < 4; i++ {
		sum += txBuf[i]
	}
	txBuf[4] = sum // 覆盖第5字节为校验和

	_, err := s.Write(txBuf)
	time.Sleep(time.Millisecond * 15) // 给内置虚拟串口缓冲消纳留出 15ms 间隙
	return err
}

func executeStringProvision(s *serial.Port, cmd byte, text string) {
	// 逐个字节喷吐发送
	for _, ch := range []byte(text) {
		_ = sendStreamByte(s, cmd, ch)
	}

	// 发送结束信标 0x00 触发 ESP32 端的状态机结束并进行 NVS 固化
	txBuf := []byte{0xAA, 0xBB, cmd, 0x00, 0x00}
	var sum byte
	for i := 0; i < 4; i++ {
		sum += txBuf[i]
	}
	txBuf[4] = sum
	_, _ = s.Write(txBuf)

	// 阻塞等待 5 字节的最终成功存储 ACK 回执
	rxBuf := make([]byte, 5)
	_, _ = io.ReadFull(s, rxBuf)
}

func main() {
	port := flag.String("port", "/dev/cu.usbmodem1101", "串口设备节点路径")
	action := flag.String("action", "read", "执行动作: read (读取当前) 或 setup (配置新网络)")
	flag.Parse()

	config := &serial.Config{Name: *port, Baud: 115200, ReadTimeout: time.Second * 3}
	s, err := serial.OpenPort(config)
	if err != nil {
		log.Fatalf("无法打开串口: %v", err)
	}
	defer s.Close()

	time.Sleep(time.Second * 2)
	s.Flush()

	switch *action {
	case "read":
		fmt.Println("[SAR-CLI] 正在请求读取当前设备的 Wi-Fi SSID 存储状态...")
		txBuf := []byte{0xAA, 0xBB, CmdReadSSID, 0x00, 0x00}
		var sum byte
		for i := 0; i < 4; i++ {
			sum += txBuf[i]
		}
		txBuf[4] = sum
		_, _ = s.Write(txBuf)

		// 异步读取 ESP32 通过 printf 吐出来的当前配网信息文本
		reader := bufio.NewReader(s)
		for i := 0; i < 15; i++ {
			line, _ := reader.ReadString('\n')
			if strings.Contains(line, "[CURRENT_WIFI]") {
				fmt.Printf("🎯 设备当前配置: %s", line)
				return
			}
		}
		fmt.Println("⚠️  未能读取到 SSID 信息，可能设备当前正处于低功耗睡眠中。")

	case "setup": // ⚙️ 修正：使用冒号而非大括号
		reader := bufio.NewReader(os.Stdin)
		fmt.Print("⌨️ 请输入搜救基站 Wi-Fi 热点名称 (SSID): ")
		ssid, _ := reader.ReadString('\n')
		ssid = strings.TrimSpace(ssid)

		fmt.Print("⌨️ 请输入热点安全密码 (Password): ")
		password, _ := reader.ReadString('\n')
		password = strings.TrimSpace(password)

		fmt.Println("\n[SAR-CLI] 正在向 ESP32 节点注入网络配网流数据...")
		executeStringProvision(s, CmdWriteSSID, ssid)
		fmt.Println("   >>> Wi-Fi 热点名称发送成功并持久化。")

		executeStringProvision(s, CmdWritePass, password)
		fmt.Println("   >>> Wi-Fi 访问密码发送成功并持久化。")

		fmt.Println("🚀 配网结束！您可以对 ESP32 进行断电重启验证任务。")

	default:
		fmt.Println("未知 Action。可供选择的操作为 -action=read 或 -action=setup")
	}
}
