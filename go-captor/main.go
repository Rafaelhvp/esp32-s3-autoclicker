package main

import (
	"encoding/json"
	"log"
	"net/http"
	"net/url"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

type Resp struct {
	Ok  bool   `json:"ok"`
	Err string `json:"error,omitempty"`
}

func logWrap(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		log.Printf("%s %s from %s\n", r.Method, r.URL.String(), r.RemoteAddr)
		next.ServeHTTP(w, r)
	})
}

func withCORS(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(204)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func btnNumber(button string) string {
	switch button {
	case "right":
		return "3"
	case "middle":
		return "2"
	default:
		return "1"
	}
}

func health(w http.ResponseWriter, r *http.Request) { w.Write([]byte(`{"ok":true}`)) }

func pos(w http.ResponseWriter, r *http.Request) {
	out, err := exec.Command("xdotool", "getmouselocation", "--shell").Output()
	if err != nil {
		log.Println("xdotool pos error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	s := string(out)
	var x, y int
	for _, line := range strings.Split(s, "\n") {
		if strings.HasPrefix(line, "X=") {
			x, _ = strconv.Atoi(strings.TrimPrefix(line, "X="))
		}
		if strings.HasPrefix(line, "Y=") {
			y, _ = strconv.Atoi(strings.TrimPrefix(line, "Y="))
		}
	}
	_ = json.NewEncoder(w).Encode(map[string]int{"x": x, "y": y})
}

func capture(w http.ResponseWriter, r *http.Request) {
	d := 3
	if s := r.URL.Query().Get("delay"); s != "" {
		if v, err := strconv.Atoi(s); err == nil && v >= 0 && v <= 30 {
			d = v
		}
	}
	time.Sleep(time.Duration(d) * time.Second)
	pos(w, r)
}

func move(w http.ResponseWriter, r *http.Request) {
	dx, _ := strconv.Atoi(r.URL.Query().Get("dx"))
	dy, _ := strconv.Atoi(r.URL.Query().Get("dy"))
	log.Printf("move dx=%d dy=%d", dx, dy)
	if err := exec.Command("xdotool", "mousemove_relative", "--", strconv.Itoa(dx), strconv.Itoa(dy)).Run(); err != nil {
		log.Println("xdotool move error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	_ = json.NewEncoder(w).Encode(Resp{Ok: true})
}

func click(w http.ResponseWriter, r *http.Request) {
	x, _ := strconv.Atoi(r.URL.Query().Get("x"))
	y, _ := strconv.Atoi(r.URL.Query().Get("y"))
	button := r.URL.Query().Get("button")
	if button == "" {
		button = "left"
	}
	dbl := r.URL.Query().Get("double") == "1"
	moveOnly := r.URL.Query().Get("move_only") == "1"

	btn := btnNumber(button)
	log.Printf("click x=%d y=%d btn=%s dbl=%v moveOnly=%v", x, y, button, dbl, moveOnly)

	if err := exec.Command("xdotool", "mousemove", strconv.Itoa(x), strconv.Itoa(y)).Run(); err != nil {
		log.Println("xdotool move error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	if !moveOnly {
		if dbl {
			_ = exec.Command("xdotool", "click", btn).Run()
			_ = exec.Command("xdotool", "click", btn).Run()
		} else {
			_ = exec.Command("xdotool", "click", btn).Run()
		}
	}
	_ = json.NewEncoder(w).Encode(Resp{Ok: true})
}

func down(w http.ResponseWriter, r *http.Request) {
	button := r.URL.Query().Get("button")
	if button == "" {
		button = "left"
	}
	if err := exec.Command("xdotool", "mousedown", btnNumber(button)).Run(); err != nil {
		log.Println("xdotool down error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	_ = json.NewEncoder(w).Encode(Resp{Ok: true})
}

func up(w http.ResponseWriter, r *http.Request) {
	button := r.URL.Query().Get("button")
	if button == "" {
		button = "left"
	}
	if err := exec.Command("xdotool", "mouseup", btnNumber(button)).Run(); err != nil {
		log.Println("xdotool up error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	_ = json.NewEncoder(w).Encode(Resp{Ok: true})
}

func drag(w http.ResponseWriter, r *http.Request) {
	x1, _ := strconv.Atoi(r.URL.Query().Get("x1"))
	y1, _ := strconv.Atoi(r.URL.Query().Get("y1"))
	x2, _ := strconv.Atoi(r.URL.Query().Get("x2"))
	y2, _ := strconv.Atoi(r.URL.Query().Get("y2"))
	dur, _ := strconv.Atoi(r.URL.Query().Get("duration")) // ms
	steps, _ := strconv.Atoi(r.URL.Query().Get("steps"))
	button := r.URL.Query().Get("button")
	if button == "" {
		button = "left"
	}
	if steps <= 0 {
		steps = 30
	}
	if dur < 0 {
		dur = 0
	}

	if err := exec.Command("xdotool", "mousemove", strconv.Itoa(x1), strconv.Itoa(y1)).Run(); err != nil {
		log.Println("xdotool move(start) error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	if err := exec.Command("xdotool", "mousedown", btnNumber(button)).Run(); err != nil {
		log.Println("xdotool mousedown error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}

	sleepPer := time.Duration(0)
	if dur > 0 && steps > 0 {
		sleepPer = time.Duration(dur/steps) * time.Millisecond
	}
	for i := 1; i <= steps; i++ {
		ix := x1 + (x2-x1)*i/steps
		iy := y1 + (y2-y1)*i/steps
		_ = exec.Command("xdotool", "mousemove", strconv.Itoa(ix), strconv.Itoa(iy)).Run()
		if sleepPer > 0 {
			time.Sleep(sleepPer)
		}
	}

	if err := exec.Command("xdotool", "mouseup", btnNumber(button)).Run(); err != nil {
		log.Println("xdotool mouseup error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	_ = json.NewEncoder(w).Encode(Resp{Ok: true})
}

func typeText(w http.ResponseWriter, r *http.Request) {
	txt := r.URL.Query().Get("text")
	if t, err := url.QueryUnescape(txt); err == nil {
		txt = t
	}
	if err := exec.Command("xdotool", "type", "--clearmodifiers", "--delay", "10", txt).Run(); err != nil {
		log.Println("xdotool type error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	_ = json.NewEncoder(w).Encode(Resp{Ok: true})
}

func keyPress(w http.ResponseWriter, r *http.Request) {
	code := r.URL.Query().Get("code")
	if code == "" {
		code = "Return"
	}
	if err := exec.Command("xdotool", "key", "--clearmodifiers", code).Run(); err != nil {
		log.Println("xdotool key error:", err)
		_ = json.NewEncoder(w).Encode(Resp{Ok: false, Err: err.Error()})
		return
	}
	_ = json.NewEncoder(w).Encode(Resp{Ok: true})
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/health", health)
	mux.HandleFunc("/pos", pos)
	mux.HandleFunc("/capture", capture)
	mux.HandleFunc("/move", move)
	mux.HandleFunc("/click", click)
	mux.HandleFunc("/down", down)
	mux.HandleFunc("/up", up)
	mux.HandleFunc("/drag", drag)
	mux.HandleFunc("/type", typeText)
	mux.HandleFunc("/key", keyPress)

	addr := "0.0.0.0:5005"
	log.Println("listening on", addr)
	if err := http.ListenAndServe(addr, withCORS(logWrap(mux))); err != nil {
		log.Fatal(err)
	}
}
