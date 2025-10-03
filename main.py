import torch
import torchvision
from torchvision.models.detection import fasterrcnn_resnet50_fpn, FasterRCNN_ResNet50_FPN_Weights
from torchvision.transforms import functional as F
import cv2
import numpy as np
from PIL import Image

# Параметры
KNOWN_WIDTH_DRONE = 15.0  # Реальный размер телефона в см (ширина)
KNOWN_WIDTH_BEACON = 10.0  # Реальный размер ArUco-маркера в см
FOCAL_LENGTH = 600  # Калибруйте заранее: (pixel_width * distance) / real_width
EMA_ALPHA = 0.3  # Коэффициент сглаживания (0 < alpha < 1; меньше — плавнее)
FRAME_SIZE = (640, 480)  # Уменьшенное разрешение для скорости
COCO_CLASSES = {77: 'cell phone'}  # Только телефон

# Проверка cv2.aruco
try:
    import cv2.aruco
    ARUCO_AVAILABLE = True
    ARUCO_DICT = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_6X6_250)
    ARUCO_PARAMS = cv2.aruco.DetectorParameters()
except ImportError:
    print("Warning: cv2.aruco not found. Install opencv-contrib-python.")
    ARUCO_AVAILABLE = False

# Загрузка моделей
device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
model = fasterrcnn_resnet50_fpn(weights=FasterRCNN_ResNet50_FPN_Weights.DEFAULT)
model.to(device)
model.eval()

midas = torch.hub.load('intel-isl/MiDaS', 'MiDaS_small')
midas.to(device)
midas.eval()
midas_transforms = torch.hub.load('intel-isl/MiDaS', 'transforms')
transform = midas_transforms.small_transform

# Переменные для трекинга и сглаживания
last_drone_box = None
last_drone_center = None
last_distance_drone = None
depth_frame_counter = 0  # Считаем кадры для редкого вызова MiDaS

# Функция для экспоненциального сглаживания
def ema_smooth(new_value, old_value, alpha=EMA_ALPHA):
    if old_value is None:
        return new_value
    return alpha * new_value + (1 - alpha) * old_value

# Функция детекции и оценки
def detect_and_estimate(frame):
    global last_drone_box, last_drone_center, last_distance_drone, depth_frame_counter
    
    # Уменьшаем разрешение
    frame = cv2.resize(frame, FRAME_SIZE)
    img = Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
    img_tensor = F.to_tensor(img).unsqueeze(0).to(device)
    
    # Детекция телефона
    with torch.no_grad():
        predictions = model(img_tensor)[0]
    
    boxes = predictions['boxes'].cpu().numpy()
    labels = predictions['labels'].cpu().numpy()
    scores = predictions['scores'].cpu().numpy()
    
    drone_box = None
    for i in range(len(scores)):
        if scores[i] > 0.7 and labels[i] == 77:  # Только 'cell phone'
            drone_box = boxes[i]
            break
    
    # Если телефон не найден, используем последнюю позицию
    if drone_box is None and last_drone_box is not None:
        drone_box = last_drone_box
    elif drone_box is None:
        return frame, None, None
    
    # Сглаживание bounding box
    if last_drone_box is not None:
        drone_box = ema_smooth(drone_box, last_drone_box)
    last_drone_box = drone_box
    
    # Позиция дрона (центр)
    drone_center = ((drone_box[0] + drone_box[2]) / 2, (drone_box[1] + drone_box[3]) / 2)
    drone_center = ema_smooth(np.array(drone_center), last_drone_center)
    last_drone_center = drone_center
    
    # Расстояние до дрона
    pixel_width_drone = drone_box[2] - drone_box[0]
    distance_to_drone = (KNOWN_WIDTH_DRONE * FOCAL_LENGTH) / pixel_width_drone if pixel_width_drone > 0 else 0
    distance_to_drone = ema_smooth(distance_to_drone, last_distance_drone)
    last_distance_drone = distance_to_drone
    
    # Depth estimation (реже, каждые 5 кадров)
    depth_drone = None
    depth_pred = None
    if depth_frame_counter % 5 == 0:
        input_batch = transform(np.array(img)).to(device)
        with torch.no_grad():
            depth_pred = midas(input_batch)
            depth_pred = torch.nn.functional.interpolate(
                depth_pred.unsqueeze(1),
                size=img.size[::-1],
                mode="bicubic",
                align_corners=False,
            ).squeeze().cpu().numpy()
        
        x1, y1, x2, y2 = map(int, drone_box)
        depth_drone = np.mean(depth_pred[y1:y2, x1:x2])
    
    depth_frame_counter += 1
    
    # Детекция ArUco-маркеров
    beacon_boxes = []
    distances_to_beacons = []
    if ARUCO_AVAILABLE:
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        corners, ids, _ = cv2.aruco.detectMarkers(gray, ARUCO_DICT, parameters=ARUCO_PARAMS)
        if ids is not None:
            for i in range(len(ids)):
                c = corners[i][0]
                x_min, y_min = np.min(c, axis=0).astype(int)
                x_max, y_max = np.max(c, axis=0).astype(int)
                beacon_boxes.append([x_min, y_min, x_max, y_max])
                
                pixel_width_beacon = x_max - x_min
                distance_to_beacon = (KNOWN_WIDTH_BEACON * FOCAL_LENGTH) / pixel_width_beacon if pixel_width_beacon > 0 else 0
                
                beacon_center = ((x_min + x_max) / 2, (y_min + y_max) / 2)
                if depth_pred is not None:
                    depth_beacon = np.mean(depth_pred[y_min:y_max, x_min:x_max])
                    scale_factor = distance_to_drone / FOCAL_LENGTH
                    dx = abs(drone_center[0] - beacon_center[0]) * scale_factor
                    dy = abs(drone_center[1] - beacon_center[1]) * scale_factor
                    dz = abs(depth_drone - depth_beacon) * 0.1 if depth_drone is not None else 0
                    dist_3d = np.sqrt(dx**2 + dy**2 + dz**2)
                    distances_to_beacons.append(dist_3d)
    
    # Отрисовка
    x1, y1, x2, y2 = map(int, drone_box)
    cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
    cv2.putText(frame, f'Phone dist: {distance_to_drone:.2f} cm', (x1, y1-10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)
    
    if beacon_boxes:
        for i, beacon_box in enumerate(beacon_boxes):
            bx1, by1, bx2, by2 = map(int, beacon_box)
            cv2.rectangle(frame, (bx1, by1), (bx2, by2), (255, 0, 0), 2)
            if i < len(distances_to_beacons):
                cv2.putText(frame, f'Beacon {i+1}: {distances_to_beacons[i]:.2f} cm', (bx1, by1-10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 0, 0), 2)
    else:
        cv2.putText(frame, 'No beacons detected', (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 255), 2)
    
    return frame, distance_to_drone, distances_to_beacons

# Основной цикл
cap = cv2.VideoCapture(0)
while True:
    ret, frame = cap.read()
    if not ret:
        break
    
    processed_frame, dist_drone, dists_beacons = detect_and_estimate(frame)
    cv2.imshow('Phone Tracking', processed_frame)
    
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

cap.release()
cv2.destroyAllWindows()