package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

// Hub maintains the set of active WebSocket clients and broadcasts messages.
type Hub struct {
	clients    map[*Client]bool
	broadcast  chan hubMessage
	register   chan *Client
	unregister chan *Client
	mu         sync.RWMutex
}

type hubMessage struct {
	dataType int // websocket.TextMessage or websocket.BinaryMessage
	data     []byte
}

type Client struct {
	hub  *Hub
	conn *websocket.Conn
	send chan hubMessage
}

func NewHub() *Hub {
	return &Hub{
		clients:    make(map[*Client]bool),
		broadcast:  make(chan hubMessage, 256),
		register:   make(chan *Client),
		unregister: make(chan *Client),
	}
}

func (h *Hub) Run() {
	for {
		select {
		case client := <-h.register:
			h.mu.Lock()
			h.clients[client] = true
			h.mu.Unlock()
			tuiLog("WebSocket client connected (%d total)", len(h.clients))
		case client := <-h.unregister:
			h.mu.Lock()
			if _, ok := h.clients[client]; ok {
				delete(h.clients, client)
				close(client.send)
			}
			h.mu.Unlock()
			tuiLog("WebSocket client disconnected (%d total)", len(h.clients))
		case message := <-h.broadcast:
			h.mu.RLock()
			for client := range h.clients {
				select {
				case client.send <- message:
				default:
					close(client.send)
					delete(h.clients, client)
				}
			}
			h.mu.RUnlock()
		}
	}
}

func (h *Hub) BroadcastText(data []byte) {
	h.broadcast <- hubMessage{websocket.TextMessage, data}
}

func (h *Hub) BroadcastBinary(data []byte) {
	h.broadcast <- hubMessage{websocket.BinaryMessage, data}
}

func serveWs(hub *Hub, w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		tuiLog("WebSocket upgrade error: %v", err)
		return
	}
	client := &Client{hub: hub, conn: conn, send: make(chan hubMessage, 256)}
	hub.register <- client

	go client.writePump()
	go client.readPump()
}

func (c *Client) readPump() {
	defer func() {
		c.hub.unregister <- c
		c.conn.Close()
	}()
	c.conn.SetReadLimit(512)
	for {
		_, _, err := c.conn.ReadMessage()
		if err != nil {
			break
		}
	}
}

func (c *Client) writePump() {
	defer c.conn.Close()
	for msg := range c.send {
		if err := c.conn.WriteMessage(msg.dataType, msg.data); err != nil {
			return
		}
	}
}

func StartServer(hub *Hub, publicDir string, port int) error {
	mux := http.NewServeMux()

	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		serveWs(hub, w, r)
	})

	mux.HandleFunc("/api/smtc", func(w http.ResponseWriter, r *http.Request) {
		resp, err := http.Get("http://localhost:9863/json")
		if err != nil {
			http.Error(w, `{"error":"SMTC service unavailable"}`, http.StatusServiceUnavailable)
			return
		}
		defer resp.Body.Close()
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Access-Control-Allow-Origin", "*")
		io.Copy(w, resp.Body)
	})

	mux.HandleFunc("/api/thumbnail", func(w http.ResponseWriter, r *http.Request) {
		resp, err := http.Get("http://localhost:9863/")
		if err != nil {
			http.Error(w, "SMTC service unavailable", http.StatusServiceUnavailable)
			return
		}
		defer resp.Body.Close()
		protoBytes, err := io.ReadAll(resp.Body)
		if err != nil {
			http.Error(w, "Failed to read thumbnail", http.StatusInternalServerError)
			return
		}
		thumb := extractThumbnail(protoBytes)
		if thumb == nil {
			http.Error(w, "No thumbnail available", http.StatusNotFound)
			return
		}
		w.Header().Set("Content-Type", "image/jpeg")
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Cache-Control", "no-cache")
		w.Write(thumb)
	})

	fs := http.FileServer(http.Dir(publicDir))
	mux.Handle("/", fs)

	addr := fmt.Sprintf(":%d", port)
	tuiLog("HTTP server listening on %s", addr)
	return http.ListenAndServe(addr, mux)
}

// Poll SMTC data from local SMTC HTTP server and broadcast via WebSocket.
func PollSmtcAndBroadcast(hub *Hub) {
	for {
		resp, err := http.Get("http://localhost:9863/json")
		if err != nil {
			time.Sleep(1 * time.Second)
			continue
		}
		body, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil {
			time.Sleep(1 * time.Second)
			continue
		}

		var smtcData map[string]interface{}
		if json.Unmarshal(body, &smtcData) == nil {
			wrapped := map[string]interface{}{
				"type": "smtc",
				"data": smtcData,
			}
			if wrappedJSON, err := json.Marshal(wrapped); err == nil {
				hub.BroadcastText(wrappedJSON)
			}
		}
		time.Sleep(200 * time.Millisecond)
	}
}

// readVarint reads a protocol buffer varint from data.
func readVarint(data []byte) (uint64, int) {
	var result uint64
	for i := 0; i < 10 && i < len(data); i++ {
		b := data[i]
		result |= uint64(b&0x7F) << (i * 7)
		if b < 0x80 {
			return result, i + 1
		}
	}
	return 0, -1
}

// extractThumbnail extracts the thumbnail bytes (field 7, wire type 2) from a SmtcData protobuf message.
func extractThumbnail(protoBytes []byte) []byte {
	pos := 0
	for pos < len(protoBytes) {
		tag, n := readVarint(protoBytes[pos:])
		if n <= 0 {
			break
		}
		pos += n

		fieldNum := tag >> 3
		wireType := tag & 0x7

		switch wireType {
		case 0: // varint
			_, n := readVarint(protoBytes[pos:])
			if n <= 0 {
				return nil
			}
			pos += n
		case 1: // 64-bit
			pos += 8
		case 2: // length-delimited
			length, n := readVarint(protoBytes[pos:])
			if n <= 0 {
				return nil
			}
			pos += n
			l := int(length)
			if pos+l > len(protoBytes) {
				return nil
			}
			if fieldNum == 7 {
				return protoBytes[pos : pos+l]
			}
			pos += l
		case 5: // 32-bit
			pos += 4
		default:
			return nil
		}
	}
	return nil
}
