# ReallySmartHouse

Base repository for a 6-node IoT smart home project using PlatformIO, the Arduino framework, and GitHub.

## Repository Structure

```text
ReallySmartHouse/
├── README.md
├── docs/
├── gateway/
├── pet-tracker/
├── plant-node/
├── air-node/
└── mailbox/
```

Each node folder is an independent PlatformIO project and must contain its own `platformio.ini`.

## Team Roles

- **Gateway** — Central gateway, LoRa receiver, cloud/Blynk forwarding
- **Pet Tracker** — GPS + BLE node
- **Aquarium** — Turbidity, pH, water temperature, feeder control
- **Plant Node** — Light, temperature, humidity, soil moisture
- **Air Node** — Temperature/humidity monitoring and climate control
- **Mailbox** — Motion + weight detection, solar-powered node

## Requirements

- Visual Studio Code
- PlatformIO IDE extension
- Git


## How to Use This Repository

### 1. Clone the repository

```bash
git clone https://github.com/JAVIERTEL/ReallySmartHouse.git
cd ReallySmartHouse
```

### 2. Open your node in PlatformIO

Do **not** open the repository root as a single PlatformIO project.

Open PlatformIO Home in VS Code and click **Pick a Folder**, then select your own node folder:

- `gateway`
- `pet-tracker`
- `aquarium`
- `plant-node`
- `air-node`
- `mailbox`

Example:

```text
ReallySmartHouse/pet-tracker
```

PlatformIO will detect the project using the `platformio.ini` file inside that folder.

### 3. Build and upload

Inside your node project you can:

- Build the firmware
- Upload it to the board
- Open the serial monitor

### 4. Git workflow

Use one shared GitHub repository for the whole team, but work in separate branches.

Recommended branches:

- `master` → stable code only
- `develop` → integration branch
- `feature/<node-name>` → personal feature branches

Example:

```bash
git checkout -b feature/pet-tracker-gps
```

### 5. Daily workflow

```bash
git pull origin main
git checkout -b feature/your-feature
git add .
git commit -m "feat(node): short description"
git push -u origin feature/your-feature
```

Then open a Pull Request on GitHub.

## Rules for the Team

- Do not push directly to `master`
- Open Pull Requests for changes
- Keep each node isolated in its own folder
- Only modify shared protocol files after team agreement
- Never commit secrets, passwords, Wi-Fi credentials, or tokens

## Notes

This repository is organized as a monorepo:
- one GitHub repository
- multiple independent PlatformIO projects
- one project folder per node

This makes it easier for the team to work in parallel without breaking each other’s build setup.
