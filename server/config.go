package main

import (
	"encoding/json"
	"fmt"
	"os"
)

// Config 是服务端运行配置；首次启动会写出默认 config.json。
type Config struct {
	HTTPAddr string `json:"http_addr"` // 监听地址，如 ":8080"
	UDPPort  int    `json:"udp_port"`  // 设备广播发现的端口
	Name     string `json:"name"`      // 广播里给设备看的名字
	DataDir  string `json:"data_dir"`  // SQLite 与 EBF 文件的存放目录
}

func defaultConfig() Config {
	return Config{
		HTTPAddr: ":8080",
		UDPPort:  9527,
		Name:     "ClockReader 书房",
		DataDir:  "./data",
	}
}

// loadConfig 读取配置；文件不存在时写出一份默认配置，方便用户手改。
func loadConfig(path string) (Config, error) {
	cfg := defaultConfig()

	raw, err := os.ReadFile(path)
	if err != nil {
		if !os.IsNotExist(err) {
			return cfg, fmt.Errorf("读取配置失败: %w", err)
		}
		buf, _ := json.MarshalIndent(cfg, "", "  ")
		if werr := os.WriteFile(path, buf, 0o644); werr != nil {
			return cfg, fmt.Errorf("写默认配置失败: %w", werr)
		}
		return cfg, nil
	}

	if err := json.Unmarshal(raw, &cfg); err != nil {
		return cfg, fmt.Errorf("解析配置失败: %w", err)
	}
	if cfg.HTTPAddr == "" {
		cfg.HTTPAddr = ":8080"
	}
	if cfg.UDPPort == 0 {
		cfg.UDPPort = 9527
	}
	if cfg.DataDir == "" {
		cfg.DataDir = "./data"
	}
	return cfg, nil
}
