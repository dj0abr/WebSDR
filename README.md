# Web-Based SDR for SDRplay RSP1A and RSP1B

This project provides a web-based Software-Defined Radio (SDR) interface for the SDRplay RSP1A and RSP1B, accessible from both desktop and mobile devices. It operates on Linux systems for both x86 and ARM architectures, making it suitable for PCs and SBCs (Single Board Computers) like the Raspberry Pi, Odroid, and others.

**Try it out:** A live demo of this WebSDR is running at the relay station **DB0SL**, and you can access it [here](http://www.db0sl.de) _(opens in a new tab)_.

## Key Features

- **Platform Compatibility**: Runs on Linux x86 and ARM.
- **Device Compatibility**: Supports PCs and SBCs (e.g., Raspberry Pi, Odroid).
- **Browser Accessibility**: Works with any modern web browser.
- **Network Access**: Uses a WebSocket on port 9001 (requires port forwarding for internet access).
- **Frequency Range**: Covers all amateur radio bands from 630m up to 70cm, including the 27 MHz CB band and the 446 MHz PMR band.

## Getting Started

### System Preparation

1. **Run the Setup Script**: Execute the `prepare` script to install all required libraries.
2. **Install SDRplay API**: Ensure that SDRplay API version 3.15 or newer is installed. You can download this from the [SDRplay website](https://www.sdrplay.com/).
   
### Building the Software

1. **Compile**: Run `make` in the project directory to build the software.
2. **Configure Apache**:
   - Copy the web files from the `./html` folder to the Apache web server directory: `/var/www/html`.
   - Ensure Apache has PHP support:
     ```bash
     sudo apt update
     sudo apt install php libapache2-mod-php
     ```
   - Restart Apache to apply changes:
     ```bash
     sudo systemctl restart apache2
     ```

### Running the Software

- Start the application by executing `./kwWebSDR`.
- For background operation, run: `./kwWebSDR &`.
- On the first run, check the terminal output to confirm that the SDRplay device is detected correctly.

### Accessing the Interface

1. Open a web browser on any device connected to the same network.
2. Enter the IP address of the Linux machine running the software to load the Web SDR GUI.

## Usage Instructions

1. **Enter Your Callsign**: Start by entering your callsign to identify yourself.
2. **Select Band and Mode**: Choose a band and operational mode.
3. **Enable Audio**: Turn on audio for real-time listening.
4. **Frequency Selection**:
   - The **Upper Waterfall** displays the entire band—click on it to select a rough frequency.
   - The **Lower Waterfall** offers a zoomed view of ±24 kHz around the selected frequency for fine-tuning.
5. **Fine Tuning**: Use the mouse wheel for precise frequency adjustments (see instructions below the waterfall).

## Band Selection

Since the SDRplay RSP1A/B can only receive one band at a time, the selected band applies to all active users. **Band selection is restricted when more than one user is logged in.** When only one user is active, they can switch bands freely.

## Technical Requirements

- Linux (x86 or ARM-based)
- Apache server with PHP support
- SDRplay API version 3.15 or newer
- Modern web browser for access

## Notes

- Up to 20 users can receive simultaneously within a selected band, with each user able to choose their individual frequency within that band.
- If you intend to access the SDR from the internet, configure port forwarding on port 9001 in your router.

---

Enjoy real-time SDR reception with this web-based SDR solution, perfect for amateur radio enthusiasts and professionals alike!
