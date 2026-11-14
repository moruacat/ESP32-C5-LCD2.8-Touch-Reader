// Package discover 用 UDP 广播让设备找到服务端。
//
// 协议（纯 ASCII，解析不用 JSON）：
//
//	设备 → 广播 255.255.255.255:9527
//	  "CLOCKREADER?DISCOVER=1"
//	服务端 → 单播回复
//	  "CLOCKREADER!URL=http://192.168.1.50:8080;NAME=ClockReader 书房"
//
// 选它而不是 mDNS：设备端只需一个 UDP socket（约 2KB），
// 而 mDNS 组件在固件里占 10-20KB 内部 RAM，已被移除过。
package discover

import (
	"fmt"
	"log"
	"net"
	"strings"
)

const (
	requestPrefix = "CLOCKREADER?DISCOVER=1"
	replyPrefix   = "CLOCKREADER!URL="
	// 单次回复的最大包长（UDP 不保证送达，设备端会重试）
	maxPacket = 512
)

// Run 监听 UDP 端口并回应发现请求。阻塞运行，调用方放到 goroutine 里。
func Run(port int, name, httpAddr string) error {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: port})
	if err != nil {
		return fmt.Errorf("UDP 监听 %d 失败: %w", port, err)
	}
	defer conn.Close()

	log.Printf("UDP 发现服务监听 :%d（设备广播 %q 即可）", port, requestPrefix)

	buf := make([]byte, maxPacket)
	for {
		n, addr, err := conn.ReadFromUDP(buf)
		if err != nil {
			log.Printf("UDP 读取失败: %v", err)
			continue
		}
		msg := strings.TrimSpace(string(buf[:n]))
		if msg != requestPrefix {
			continue // 不是我们的协议，忽略
		}

		reply := buildReply(httpAddr, name)
		if _, err := conn.WriteToUDP([]byte(reply), addr); err != nil {
			log.Printf("UDP 回复 %v 失败: %v", addr, err)
			continue
		}
		log.Printf("发现请求来自 %v，已回复 %s", addr, reply)
	}
}

// buildReply 把监听地址转成设备能直接用的 URL。
// httpAddr 可能是 ":8080"（监听所有网卡），此时 URL 里的主机名留空，
// 由设备用收到回复的源地址补全——服务端通常不知道自己在局域网里的 IP。
func buildReply(httpAddr, name string) string {
	host := ""
	port := "8080"
	if i := strings.LastIndex(httpAddr, ":"); i >= 0 {
		host = httpAddr[:i]
		port = httpAddr[i+1:]
	}

	url := "http://" + host + ":" + port
	reply := replyPrefix + url + ";NAME=" + sanitize(name)
	if len(reply) > maxPacket {
		reply = reply[:maxPacket]
	}
	return reply
}

// sanitize 去掉会破坏协议的分隔符。
func sanitize(s string) string {
	r := strings.NewReplacer(";", ",", "\n", " ", "\r", " ")
	return r.Replace(s)
}
