# FreeRTOS-IoT-Project


A robust, real-time IoT solution built using the ESP-IDF framework. This project serves as a bridge between low-level hardware sensors and high-level web interfaces, utilizing a dual-core architecture to ensure data integrity and system responsiveness.
📽️ Project Demo

In this video: Watch the real-time temperature updates and the instant "!!! OBSTACLE !!!" alert when the IR sensor is triggered.
________________________________________
🛠️ Hardware Specifications & Pin Mapping
I designed this system using the following physical configuration:
Component	ESP32        Pin	      Protocol / Mode
DHT11 (Temp/Hum)	     GPIO 4	    Custom 1-Wire (Bit-Banging)
IR Obstacle Sensor	   GPIO 27	  Digital Input + Hardware ISR
________________________________________
🏗️ Software Architecture
The system is built on a Producer-Consumer model using FreeRTOS:
1. Sensing Layer (Producer - Core 1)
•	Custom Driver: I authored a manual bit-banging driver for the DHT11, utilizing esp_rom_delay_us for the critical 20ms start pulse and sub-100us bit sampling.
•	Concurrency Protection: Wrapped the 1-Wire protocol in a Critical Section (taskENTER_CRITICAL) to prevent Wi-Fi radio interrupts from corrupting the timing.
•	Task Isolation: Pinned to Core 1 to ensure the bit-banging logic has dedicated CPU cycles.
2. Communication Layer (Consumer - Core 0)
•	Event-Driven Networking: Used an Event Group to synchronize the web server start-up with the DHCP "Got IP" event.
•	Thread-Safe Data Handling: Utilized a xQueueCreate(1, ...) and xQueueOverwrite to ensure the web dashboard always displays the most recent state vector without memory overflow.
•	Interrupt Management: Implemented Deferred Interrupt Processing via a Binary Semaphore. The ISR handles the hardware trigger instantly, while a background task handles the slow logging operations.
________________________________________
🌐 Web Dashboard Features
The embedded HTTP Server (built on esp_http_server.h) provides:
•	Real-time Monitoring: Temperature and Humidity data served directly from the RTOS Queue.
•	Security Alerts: Instant status updates showing "Clear" or "!!! OBSTACLE !!!".
•	Auto-Refresh: Integrated <meta> tags for a zero-client-side-code live update experience.
________________________________________
🧠 Challenges & Solutions
•	Issue: Random -99 values on the dashboard when Wi-Fi was active.
•	Diagnosis: High-priority Wi-Fi interrupts were stealing CPU cycles during the DHT11 bit-sampling.
•	Solution: I implemented Task Pinning to separate the tasks by core and used Critical Sections to temporarily disable the scheduler during the 40-bit data collection.
________________________________________
🚀 How to Run
1.	Framework: Install ESP-IDF v5.x.
2.	Configuration: Set your SSID and Password in main.c.
3.	Build & Flash:
4.	Access: Connect to the same Wi-Fi and open the IP address shown in the terminal.
