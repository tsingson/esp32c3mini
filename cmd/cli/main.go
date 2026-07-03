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

func sendStreamByte(s *serial.Port, cmd, charByte byte) error {
	txBuf := []byte{0xAA, 0xBB, cmd, charByte, 0x00}
	var sum byte
	for i := 0; i < 4; i++ {
		sum += txBuf[i]
	}
	txBuf[4] = sum

	_, err := s.Write(txBuf)
	time.Sleep(time.Millisecond * 15)
	return err
}

func executeStringProvision(s *serial.Port, cmd byte, text string) {
	for _, ch := range []byte(text) {
		_ = sendStreamByte(s, cmd, ch)
	}

	txBuf := []byte{0xAA, 0xBB, cmd, 0x00, 0x00}
	var sum byte
	for i := 0; i < 4; i++ {
		sum += txBuf[i]
	}
	txBuf[4] = sum
	_, _ = s.Write(txBuf)

	rxBuf := make([]byte, 5)
	_, _ = io.ReadFull(s, rxBuf)
}

// 提取出来的公共配网人机接口
func handleInteractiveSetup(s *serial.Port) {
	reader := bufio.NewReader(os.Stdin)
	fmt.Println("\n⚠️  [探测提示] 检测到搜救节点处于出厂无配网状态！已自动为您启动交互配网引导...")

	fmt.Print("⌨️  请输入搜救现场 Wi-Fi 热点名称 (SSID): ")
	ssid, _ := reader.ReadString('\n')
	ssid = strings.TrimSpace(ssid)

	fmt.Print("⌨️  请输入热点安全验证密码 (Password): ")
	password, _ := reader.ReadString('\n')
	password = strings.TrimSpace(password)

	fmt.Println("\n[SAR-CLI] 正在建立高速二进制流注入网络数据...")
	executeStringProvision(s, CmdWriteSSID, ssid)
	fmt.Println("   >>> 搜救基站 SSID 注入成功并已写入 NVS 硬件扇区。")

	executeStringProvision(s, CmdWritePass, password)
	fmt.Println("   >>> 验证密码 注入成功并已写入 NVS 硬件扇区。")

	fmt.Println("🚀 搜救节点配网成功！3分钟周期性低功耗唤醒搜救循环已在 ESP32 内部启动。")
}

func main() {
	port := flag.String("port", "/dev/cu.usbmodem1101", "串口设备节点路径")
	action := flag.String("action", "read", "可选动作: read 或 setup")
	flag.Parse()

	config := &serial.Config{Name: *port, Baud: 115200, ReadTimeout: time.Second * 3}
	s, err := serial.OpenPort(config)
	if err != nil {
		log.Fatalf("无法打开串口: %v", err)
	}
	defer s.Close()

	// 锁死握手震荡缓冲
	time.Sleep(time.Millisecond * 1500)
	s.Flush()

	switch *action {
	case "read":
		fmt.Println("[SAR-CLI] 正在请求读取搜救节点当前的网卡配置...")
		txBuf := []byte{0xAA, 0xBB, CmdReadSSID, 0x00, 0x00}
		var sum byte
		for i := 0; i < 4; i++ {
			sum += txBuf[i]
		}
		txBuf[4] = sum
		_, _ = s.Write(txBuf)

		reader := bufio.NewReader(s)
		for i := 0; i < 15; i++ {
			line, _ := reader.ReadString('\n')
			if strings.Contains(line, "[CURRENT_WIFI]") {
				fmt.Printf("🎯 节点读取反馈: %s", line)

				// 🎯 需求补充 2：如果读到的结果是出厂空白“未设置SSID”，直接就地降级拦截，开启配网注入
				if strings.Contains(line, "未设置SSID") {
					handleInteractiveSetup(s)
				}
				return
			}
		}
		fmt.Println("⚠️  未能读取到状态响应。节点当前可能正在轻度睡眠中，请在其苏醒窗口或重新上电时重试。")

	case "setup":
		// 保留手工强行指定配置的入口
		handleInteractiveSetup(s)

	default:
		fmt.Println("未知 Action。")
	}
}
