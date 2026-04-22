# 🌿 Kräuter-WG - ESP32 Smart Garden
**Developed by Austrian Flame (with Gemini)**

Transform your indoor plants into a fully automated smart garden. The **Kräuter-WG** is an open-source, ESP32-based IoT solution that monitors soil moisture and handles the watering for you—no coding skills required.

Say goodbye to complex setups and hardcoded values in the Arduino IDE. Instead, enjoy a clean and responsive **Web UI** hosted directly on your local network. You can easily configure sensor thresholds, control your water pumps, and set up logical "Smart Rules" right from your smartphone browser.

<img width="1478" height="618" alt="image" src="https://github.com/user-attachments/assets/4058f71e-3979-4ef9-81b4-07de3ed2606d" />
<img width="1475" height="579" alt="image" src="https://github.com/user-attachments/assets/308e3e22-44e7-4bec-a9cb-81c79a5f1f83" />


---

## ⚠️ BETA RELEASE (v0.9.0-beta) ⚠️
This project is currently in the **public beta phase**. While the core system runs stably in my personal setup, the following features are experimental and have not been exhaustively tested:

* **Zero Coding Interface:** The dynamic web-based configuration is still in early beta and might have bugs in edge cases.
* **Smart Rules Engine:** The core logic has been tested and works, but long-term stability (running continuously over weeks/months) and highly complex overlapping rules are still under observation.
* **Adding Custom Sensors:** The UI flow for adding new sensors dynamically on the fly has not been fully verified yet.
* **Extensions / Expansions:** Adding new hardware modules or expansions to the existing system is currently untested.

If you encounter bugs, weird UI behavior, or have ideas for optimization, **please open an issue**! Your feedback is highly welcome to help get this to a stable v1.0.0.

---

## 🚀 Key Features

* **Zero Coding Required:** Once flashed, everything is controlled via the browser. No need to recompile code just to change a moisture target.
* **Smart Rules Engine:** Visually define when pumps should turn on or off based on live sensor data.
* **100% Local Network:** No cloud subscription, no data harvesting. The ESP32 hosts its own web server.
* **Live Environmental Tracking:** Monitors air temperature, humidity, and individual soil moisture levels per plant.
* **Power Bank Friendly:** Designed to run efficiently on simple hardware.

---

## 🤖 The System
The biggest pain point in DIY gardening is tuning the water flow. The Kraeuter-WG solves this with an intuitive Smart Rules menu. You set the target moisture (e.g., Target: 20%, Current: 77% -> Pump stays OFF).

<img width="1464" height="731" alt="image" src="https://github.com/user-attachments/assets/8e6d09a3-796a-4982-9ee8-e8588d6ebe7d" />

<img width="1484" height="656" alt="image" src="https://github.com/user-attachments/assets/1acf53d9-9b5f-47e7-a99a-94b364aa137d" />

<img width="1462" height="747" alt="image" src="https://github.com/user-attachments/assets/6f4e949b-1a0c-4c16-ab96-a44862d463ab" />

<img width="1473" height="714" alt="image" src="https://github.com/user-attachments/assets/184585f7-8ff1-41ba-9cc4-ecc940d047a5" />

<img width="1453" height="690" alt="image" src="https://github.com/user-attachments/assets/0d3c61aa-a8af-4522-9059-708844b12d17" />

<img width="1463" height="696" alt="image" src="https://github.com/user-attachments/assets/375afa48-52bd-4212-9422-72b139d7498d" />

<img width="1452" height="721" alt="image" src="https://github.com/user-attachments/assets/af8184c8-4121-42dc-ade4-18698af42a01" />

<img width="1475" height="727" alt="image" src="https://github.com/user-attachments/assets/83d66cf7-e940-49e2-9666-e8fe65eaadbe" />



---

## 🛠️ Hardware Requirements
To build your own Kraeuter-WG, you need:
* 1x **ESP32** Microcontroller
* Capacitive Soil Moisture Sensors
* 5V Water Pumps & Tubing
* Relay Module (for the pumps)
* Power Bank or 5V Power Supply


---

## ⚙️ Quick Start & Installation

1. **Clone the Repository:**
   ```bash
   git clone https://github.com/austrianflame/kraeuter-wg.git
   
2. Download the Required Libaries and Flash the ESP32:

   📚 Required Libraries
   Before compiling, please install the following libraries via the Arduino IDE Library Manager:

   * **WiFiManager** by tzapu
   * **Adafruit GFX Library** by Adafruit
   * **Adafruit SSD1306** by Adafruit
   * **DHT sensor library** by Adafruit *(make sure to also install the Adafruit Unified Sensor dependency)*
   * **PCF8574** by Rob Tillaart
   * **ArduinoJson** by Benoit Blanchon
   * **PubSubClient** by Nick O'Leary

   *(Note: All other included libraries like LittleFS, ESPmDNS, or esp_now are part of the standard ESP32 core and do not need to be downloaded separately.)*

   Open the Kraeuter-WG.ino file in the Arduino IDE or your preferred environment, ensure you have the necessary WiFi/Webserver libraries installed, and flash it to your board. (No LittleFS data     upload required via Arduino IDE, the core UI is contained in one file!)

4. Connect to the Network (Captive Portal):
  Power up the ESP32. On the very first boot, it opens its own WiFi Access Point. Connect to it, configure your local WiFi credentials, and then access the assigned IP address in your local         browser.

5. Configure the System:**
   Head to the Web UI, add how many plants you want, choose the pins from the dropdown menu, and get started. 
   *(Important: When using small pots, please ensure the watering time isn't set too long to prevent flooding!)*

6. Help & Presets:
  For easy setup, please read the integrated help menu. You can also upload the provided Plant preset files to set the ideal moisture percentages automatically (Note: presets are provided "as is"   with no guarantee).

7. 📁 Update Plant Database (plants.js)
   The system uses an external file called plants.js for the plant database. Why? So you can easily swap or update the list! If the community releases a new version or you want to add your own       varieties, simply upload the new file at the very bottom of the Plant Settings in the Web UI. The old list will automatically be replaced by the new one.

   
📡 Over-The-Air (OTA) Updates
This project supports OTA updates via ArduinoOTA. This means you don't need to connect the ESP32 via USB to flash a new version of the code – you can do it directly over your WiFi network!
* **OTA Password:** "Kraeuter-WG!"
*(Make sure to change this password in the `.ino` file if you are running this on a public or unsecure network!)*
---

⚖️ License & Copyright
Copyright (c) 2026 Austrian Flame

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License v3.0 (GPLv3) as published by the Free Software Foundation.

What does this mean for you?

✅ Personal & Hobby Use: You are entirely free to use, modify, and build this system for your own garden.

⚠️ Commercial Use (Copyleft): If you use this code in a commercial product, you must disclose your entire source code under the same GPLv3 license. Closed-source commercial distribution is strictly prohibited under this license.

💼 Commercial Dual-Licensing

If you are a company or individual looking to use the Kraeuter-WG system in a closed-source commercial product without the GPLv3 restrictions, please contact me directly for a commercial license agreement.

Disclaimer: This software is provided "as is", without warranty of any kind. The use of the software and hardware setup is at your own risk.

Developed with 🔥 by Austrian Flame (with Gemini)
