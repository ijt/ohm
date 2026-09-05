package main

import (
	"strings"
	"time"
)

// Enough history to cover the widest spark column after a resize without
// immediately looking empty.
const maxSparkSamples = 48

// Block elements -- one vertical level per cell. Braille is denser but
// needs a font that reliably includes the dots; these are ubiquitous.
var sparkBars = []rune("▁▂▃▄▅▆▇█")

// Per-download throughput trail. Rates are only appended when
// received_bytes advances -- DownloadManager persists ~1/sec while we poll
// every 500ms, so sampling every poll would invent alternating zero-rate
// dips and make the sparkline flicker even during a steady transfer.
type histState struct {
	rates    []float64 // bytes/sec between successive advances
	lastRecv int64
	lastAt   time.Time
}

func sparkline(rates []float64, width int) string {
	if width <= 0 {
		return ""
	}
	vals := make([]float64, width)
	if n := len(rates); n > 0 {
		src := rates
		if n > width {
			src = rates[n-width:]
		}
		copy(vals[width-len(src):], src)
	}
	max := 0.0
	for _, v := range vals {
		if v > max {
			max = v
		}
	}
	var b strings.Builder
	for _, v := range vals {
		if max <= 0 || v <= 0 {
			b.WriteRune(sparkBars[0])
			continue
		}
		idx := int(v / max * float64(len(sparkBars)-1))
		if idx < 1 {
			idx = 1 // any real activity clears the floor glyph
		}
		if idx >= len(sparkBars) {
			idx = len(sparkBars) - 1
		}
		b.WriteRune(sparkBars[idx])
	}
	return b.String()
}

func (m *model) recordHistory(ds []Download) {
	now := time.Now()
	if m.hist == nil {
		m.hist = make(map[int64]*histState)
	}
	live := make(map[int64]struct{}, len(ds))
	for _, d := range ds {
		if d.State != StateInProgress {
			continue
		}
		live[d.ID] = struct{}{}
		h := m.hist[d.ID]
		if h == nil {
			m.hist[d.ID] = &histState{lastRecv: d.ReceivedBytes, lastAt: now}
			continue
		}
		if d.ReceivedBytes == h.lastRecv {
			continue
		}
		dt := now.Sub(h.lastAt).Seconds()
		rate := 0.0
		if dt > 0 {
			rate = float64(d.ReceivedBytes-h.lastRecv) / dt
			if rate < 0 {
				rate = 0
			}
		}
		h.rates = append(h.rates, rate)
		if len(h.rates) > maxSparkSamples {
			h.rates = append([]float64(nil), h.rates[len(h.rates)-maxSparkSamples:]...)
		}
		h.lastRecv = d.ReceivedBytes
		h.lastAt = now
	}
	for id := range m.hist {
		if _, ok := live[id]; !ok {
			delete(m.hist, id)
		}
	}
}

func (m model) ratesFor(id int64) []float64 {
	if h := m.hist[id]; h != nil {
		return h.rates
	}
	return nil
}
