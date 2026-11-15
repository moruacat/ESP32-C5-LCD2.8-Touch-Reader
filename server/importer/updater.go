package importer

import (
	"context"
	"log"
	"time"
)

// StartUpdater 起一个后台协程，每 interval 检查一次番茄书有没有新章。
//
// 默认 6 小时：番茄的更新频率通常是日更，6 小时足够及时，
// 也不会给对方站点造成压力（每本书每次只发一个目录请求，
// 只有真的有新章才会去抓正文）。
func (im *Importer) StartUpdater(ctx context.Context, interval time.Duration) {
	if interval <= 0 {
		interval = 6 * time.Hour
	}

	go func() {
		// 启动后先等两分钟，避开开机时的初始化高峰
		select {
		case <-time.After(2 * time.Minute):
		case <-ctx.Done():
			return
		}

		ticker := time.NewTicker(interval)
		defer ticker.Stop()

		for {
			im.checkFanqieUpdates()

			select {
			case <-ticker.C:
			case <-ctx.Done():
				return
			}
		}
	}()

	log.Printf("番茄追更已启动：每 %s 检查一次", interval)
}

// checkFanqieUpdates 扫一遍番茄书，有更新就增量补章。
func (im *Importer) checkFanqieUpdates() {
	books, err := im.Store.ListFanqieBooks()
	if err != nil {
		log.Printf("追更：读取番茄书单失败: %v", err)
		return
	}
	if len(books) == 0 {
		return
	}

	log.Printf("追更：检查 %d 本番茄书", len(books))
	for i := range books {
		b := &books[i]

		updated, added, err := im.UpdateFanqie(b, FanqieOptions())
		if err != nil {
			log.Printf("追更《%s》失败: %v", b.Title, err)
			continue
		}
		if updated {
			log.Printf("追更《%s》：新增 %d 章，现共 %d 章", b.Title, added, b.Chapters)
			// 每本之间歇一下，避免连续请求被限频
			time.Sleep(2 * time.Second)
		}
	}
}
