import React, { useState, useEffect, useRef } from "react";
import "./App.css";

// -------------------- TYPES --------------------
interface BeaconData {
  rssi: number;
  distance: number;
  found: boolean;
}

interface Position {
  x: number;
  y: number;
  accuracy: number;
}

interface Fusion {
  wifi_weight: number;
  ble_weight: number;
}

interface ESPData {
  timestamp: number;
  position: Position;
  wifi: Record<string, BeaconData>;
  ble: Record<string, BeaconData>;
  fusion: Fusion;
  cv?: Position;
}

// -------------------- WEBSOCKET HOOK --------------------
function useWebSocket(url: string) {
  const [data, setData] = useState<ESPData | null>(null);
  const [connected, setConnected] = useState(false);
  const wsRef = useRef<WebSocket | null>(null);

  useEffect(() => {
    let ws: WebSocket;
    let reconnectTimeout: NodeJS.Timeout;

    function connect() {
      // Закрываем предыдущее подключение если есть
      if (wsRef.current) {
        wsRef.current.close();
      }

      ws = new WebSocket(url);
      wsRef.current = ws;

      ws.onopen = () => {
        setConnected(true);
        console.log("WebSocket connected");
      };
      
      ws.onmessage = (event) => {
        try {
          const message = JSON.parse(event.data);
          // Backend отправляет данные в формате {type, data, timestamp}
          if (message.type === "position_update" && message.data) {
            setData(message.data);
          }
        } catch (err) {
          console.error("Invalid JSON", err);
        }
      };
      
      ws.onclose = () => {
        setConnected(false);
        console.log("WebSocket disconnected, reconnecting in 2s...");
        reconnectTimeout = setTimeout(connect, 2000);
      };
      
      ws.onerror = (err) => {
        console.error("WebSocket error", err);
        setConnected(false);
      };
    }

    connect();
    
    return () => {
      if (reconnectTimeout) clearTimeout(reconnectTimeout);
      if (wsRef.current) {
        wsRef.current.close();
      }
    };
  }, [url]);

  return { data, connected };
}

// -------------------- MAP CANVAS --------------------
const SCALE = 50; // 1 m = 50 px

interface MapCanvasProps {
  data: ESPData | null;
  width?: number;
  height?: number;
  showHistory?: boolean;
  historyLength?: number;
}

const MapCanvas: React.FC<MapCanvasProps> = ({
  data,
  width = 600,
  height = 400,
  showHistory = true,
  historyLength = 30,
}) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const positionsRef = useRef<Position[]>([]);

  // Плавная анимация через requestAnimationFrame
  const animationRef = useRef<number>();
  const currentPosRef = useRef<Position>({ x: 0, y: 0, accuracy: 0 });

  useEffect(() => {
    if (!data) return;

    positionsRef.current.push(data.position);
    if (positionsRef.current.length > historyLength)
      positionsRef.current.shift();
  }, [data, historyLength]);

  useEffect(() => {
    const ctx = canvasRef.current?.getContext("2d");
    if (!ctx) return;

    function draw() {
      if (!ctx) return;

      // Линейная интерполяция
      if (data && data.position) {
        currentPosRef.current.x += (data.position.x - currentPosRef.current.x) * 0.1;
        currentPosRef.current.y += (data.position.y - currentPosRef.current.y) * 0.1;
        currentPosRef.current.accuracy += (data.position.accuracy - currentPosRef.current.accuracy) * 0.1;
      }

      // Очистка
      ctx.clearRect(0, 0, width, height);

      // Градиентное фоновое поле
      const gradient = ctx.createLinearGradient(0, 0, width, height);
      gradient.addColorStop(0, "#111");
      gradient.addColorStop(1, "#222");
      ctx.fillStyle = gradient;
      ctx.fillRect(0, 0, width, height);

      // Сетка
      ctx.strokeStyle = "#333";
      for (let x = 0; x <= width; x += SCALE) {
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, height);
        ctx.stroke();
      }
      for (let y = 0; y <= height; y += SCALE) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(width, y);
        ctx.stroke();
      }

      if (!data) return;

      // WiFi маяки
      Object.entries(data.wifi).forEach(([name, beacon], i) => {
        const bx = (i + 1) * SCALE * 1.5;
        const by = 50;
        ctx.fillStyle = "gold";
        ctx.strokeStyle = "gold";
        ctx.beginPath();
        ctx.arc(bx, by, 6, 0, 2 * Math.PI);
        ctx.fill();

        // Радиус сигнала
        ctx.globalAlpha = 0.2;
        ctx.beginPath();
        ctx.arc(bx, by, beacon.distance * SCALE, 0, 2 * Math.PI);
        ctx.stroke();
        ctx.globalAlpha = 1;
      });

      // BLE маяки
      Object.entries(data.ble).forEach(([name, beacon], i) => {
        const bx = (i + 1) * SCALE * 1.5;
        const by = 150;
        ctx.fillStyle = "deepskyblue";
        ctx.strokeStyle = "deepskyblue";
        ctx.beginPath();
        ctx.arc(bx, by, 6, 0, 2 * Math.PI);
        ctx.fill();

        ctx.globalAlpha = 0.2;
        ctx.beginPath();
        ctx.arc(bx, by, beacon.distance * SCALE, 0, 2 * Math.PI);
        ctx.stroke();
        ctx.globalAlpha = 1;
      });

      // История движения
      if (showHistory && positionsRef.current.length > 1) {
        ctx.strokeStyle = "rgba(0,255,255,0.5)";
        ctx.lineWidth = 2;
        ctx.beginPath();
        positionsRef.current.forEach((pos, idx) => {
          const x = pos.x * SCALE;
          const y = pos.y * SCALE;
          if (idx === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        });
        ctx.stroke();
      }

      // Текущий объект
      const posX = currentPosRef.current.x * SCALE;
      const posY = currentPosRef.current.y * SCALE;

      ctx.fillStyle = currentPosRef.current.accuracy > 2 ? "red" : "lime";
      ctx.beginPath();
      ctx.arc(posX, posY, 10, 0, 2 * Math.PI);
      ctx.fill();

      // Accuracy circle
      ctx.fillStyle = "rgba(0,255,0,0.2)";
      ctx.beginPath();
      ctx.arc(posX, posY, currentPosRef.current.accuracy * SCALE, 0, 2 * Math.PI);
      ctx.fill();

      // CV точка
      if (data.cv) {
        ctx.fillStyle = "red";
        ctx.beginPath();
        ctx.arc(data.cv.x * SCALE, data.cv.y * SCALE, 6, 0, 2 * Math.PI);
        ctx.fill();
      }

      animationRef.current = requestAnimationFrame(draw);
    }

    draw();

    return () => {
      if (animationRef.current) cancelAnimationFrame(animationRef.current);
    };
  }, [data, width, height, showHistory]);

  return <canvas ref={canvasRef} width={width} height={height} className="map-canvas" />;
};

// -------------------- SIDEBAR COMPONENTS --------------------
const InfoCard: React.FC<{ title: string }> = ({ title, children }) => (
  <div className="info-card">
    <h3>{title}</h3>
    {children}
  </div>
);

const PositionInfo: React.FC<{ position: Position | null }> = ({ position }) => {
  if (!position) return null;
  return (
    <InfoCard title="Current Position">
      <p>X: {position.x.toFixed(2)} m</p>
      <p>Y: {position.y.toFixed(2)} m</p>
      <p>Accuracy: {position.accuracy.toFixed(2)} m</p>
    </InfoCard>
  );
};

const FusionInfo: React.FC<{ fusion: Fusion | null }> = ({ fusion }) => {
  if (!fusion) return null;
  return (
    <InfoCard title="Fusion Weights">
      <div className="fusion-bar">
        <span>WiFi: {Math.round(fusion.wifi_weight * 100)}%</span>
        <div className="bar wifi" style={{ width: `${fusion.wifi_weight * 100}%` }} />
      </div>
      <div className="fusion-bar">
        <span>BLE: {Math.round(fusion.ble_weight * 100)}%</span>
        <div className="bar ble" style={{ width: `${fusion.ble_weight * 100}%` }} />
      </div>
    </InfoCard>
  );
};

const BeaconList: React.FC<{ beacons: Record<string, BeaconData> | null; type: string }> = ({ beacons, type }) => {
  if (!beacons) return null;
  return (
    <InfoCard title={`${type} Beacons`}>
      <table>
        <thead>
          <tr>
            <th>Name</th>
            <th>RSSI</th>
            <th>Distance</th>
            <th>Found</th>
          </tr>
        </thead>
        <tbody>
          {Object.entries(beacons).map(([name, b]) => (
            <tr key={name} className={!b.found ? "not-found" : ""}>
              <td>{name}</td>
              <td>{b.rssi}</td>
              <td>{b.distance.toFixed(2)}</td>
              <td>{b.found ? "✓" : "✗"}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </InfoCard>
  );
};

// -------------------- APP --------------------
export default function App() {
  const { data, connected } = useWebSocket("ws://localhost:8080/ws");

  return (
    <div className="app">
      <header className="header">
        <h1>Indoor Positioning System</h1>
        <div className={`status ${connected ? "connected" : "disconnected"}`}>
          {connected ? "Connected" : "Disconnected"}
        </div>
      </header>
      <div className="main-content">
        <MapCanvas data={data} />
        <aside className="sidebar">
          <PositionInfo position={data?.position || null} />
          <FusionInfo fusion={data?.fusion || null} />
          <BeaconList beacons={data?.wifi || null} type="WiFi" />
          <BeaconList beacons={data?.ble || null} type="BLE" />
        </aside>
      </div>
      <footer className="footer">
        <p>Legend: Green = Object, Blue = BLE, Gold = WiFi, Red = CV / High inaccuracy</p>
        <p>Scale: 1 m = {SCALE}px</p>
      </footer>
    </div>
  );
}
