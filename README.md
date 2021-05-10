# WiGot in a nutshell
WiGot is a quick effort for getting rid of all cables coming out from a Gotek drive installed in an Amiga. 
## Benefits
* No cables coming out of your Amiga
* Convenient image selection with rotary switch and OLED screen
 
 # Concept 
Up to know, most efforts focus on providing control panels with OLED screens, buttons / rotary switches, that are wired to the Gotek drive installed inside the Amiga. WiGot is a quick (and dirty) effort for getting rid of all cables coming out from a Gotek drive installed in an Amiga. It is based on connecting a small microcontoller with wireless connectivity capabilities inside the Gotek drive that emulates image selection from a rotary switch. An external wireless controller with OLED screen and a rotary switch, transmits user rotations to the internal microcontroller, which relays them to the Gotek processor.  
 
 # Prototype
 The current prototype is based on a SFRC9220 Gotek drive and two WeMos D1 EPS32 modules; the first one (A) is mounted inside a Gotek drive with HXC firmware, and the second one (B) allows users to select disk images. Communication is two-way; whenever the user rotates the switch, A informs B (including direction). B then simply uses the original Gotek rotary pins that are already available to emulate a full rotary (i.e. no grey encoding up to now). All data transfers are done using the ESP Now wireless protocol.
 
![Concept](/images/concept.png)

The image above illustates the WiGot concept, based on the two aforementioned WeMos D1 ESP32 modules, namely A and B. 

The left part shows how to connect A with the Gotek drive: 

* IO pins 17 and 33 are used to send fake rotary signals to the Gotek processor. 
* IO pins 34 and 35 are used to analog-read the voltage between the green led (L4 in the schematic), in order to identify whether the floopy is busy.
* VCC and GND pins are connected to the Gotek headers shown with red and black boxes.

The right parts shows how to build B:
* IO pins 22 and 21 are used for the I2C protocol required by the OLED.
* IO pins 17, 16 and 26 are connected to the S2, S1 and KEY pins respectevily of the KY-040 rotary switch.
* IO pins 19, 23, 18 and 5 are used for the SPI protocol required by the SD Card.

# How to use it
## First-time use
After you connect A to the Gotek drive as shown above, follow these steps:
### Create and save the data file
* Create a data.cfg file and edit it as follows:
    * the first line must contain the MAC address of A, seperated by commas. [Here](https://randomnerdtutorials.com/get-change-esp32-esp8266-mac-address-arduino/) you can see how to read the ESP MAC address. Example for MAC address CC:50:E3:B5:A1:1C in the data file:
        * CC,50,E3,B5,A1,1C 
    * each subsequent line must contain a single image's data in the form of &lt;name&gt;,&lt;disk&gt;,&lt;total disks&gt;. Example with HXC firmware and Desert Strike images: 
        * HXCFECFGV1.0,1,1
        * Desert Strike,1,3
        * Desert Strike,2,3
        * Desert Strike,3,3
* Save the data file into your FAT32 formatted SD card.
* Plug the SD card into the slot of the controller module (B).
* An example of my data.cfg file can be found ![here](/gotek_ctrl/data/data.cfg).
### Power up
Now that you have created your data.cfg file and saved it in the SD card, follow these steps:
* Power up your Amiga, A will wait for B.
* Plug B to power, and check the OLED screen; it will take a while to establish connection with A and then ask you to set the current image.
* Rotate the switch until the image number matches the one shown in the Gotek OLED screen. When done, press the rotary switch once to sync the controller with the image selected in the Gotek drive.
* That's it! From now can use the rotary switch to select images.
## Regular use
Always power on first the Amiga and then directly the wireless controller B. Wait for a moment to establish connection and then start selecting images. In case you add / remove images, you need to update the data.cfg file as well, using the instructions above.

# FAQ - Troubleshoot
* Image number does not match the one shown in the Gotek drive OLED screen
  * If, for any reason, this happens, simply press the rotary switch to set again the currently selected image from the Gotek drive. Note that A and B should be already connected before syncing the image.
* Cannot establish connection between A and B
  * Make sure that the MAC address is correct and properly saved (each number separated by commas) in the first line of the data.cfg file.
* The rotary switch does not work properly
  * The WiGot current implementation assumes the following S1 and S2 (of the KY-040) states:
    * Single clockwise rotation (increases selected image by 1): 10 → 00 → 01 → 11 → 10
    * Single counter-clockwise rotation (decreases selected image by 1): 00 → 10 → 11 → 01 → 00  
 
# WiGot images

## Mod (A)

![Gotek mod](/images/gotek_mod.jpg)

## Wireless controller (B)

![Wireless controller](/images/wireless_controller.jpg)

![Here](/gotek_ctrl/pcb) you can find a clumsy PCB made with KiCAD for the wireless controller.


