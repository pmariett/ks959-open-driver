# REVERS INGINEERING

## Structure USB très probable de la KS-959  
Un périphérique USB expose une hiérarchie standard :  
Device  
 └─ Configuration  
     └─ Interface  
         ├─ Endpoint  
         ├─ Endpoint  
         └─ Endpoint  
		 
Pour un adaptateur IrDA USB classique, on obtient généralement :  
USB Device  
│  
├─ Device Descriptor  
│   VID: 07D0  
│   PID: 4959  
│   Class: FF (Vendor Specific)  
│  
└─ Configuration 1  
     │  
     └─ Interface 0  
         Class: FF (Vendor specific)  
         Subclass: 00  
         Protocol: 00  
		 
Le point important :  
	Class = FF  

Cela signifie que le périphérique n'utilise aucune classe USB standard.  
Donc :  
- pas CDC  
- pas HID  
- pas USB IrDA bridge standard  

👉 le protocole est 100 % propriétaire.  

## Endpoints très probables  
Presque toutes les clés IrDA USB utilisent ce schéma :  
- Endpoint 0  (CONTROL)  
- Endpoint 1  (BULK OUT)  
- Endpoint 2  (BULK IN)  
soit :  
- EP0  control  
- EP1  OUT  -> envoyer trames IR  
- EP2  IN   <- recevoir trames IR  

Configuration probable :  
- Endpoint 0x01  BULK OUT  
- Endpoint 0x81  BULK IN  
ou parfois :  
- 0x02  BULK OUT  
- 0x82  BULK IN  

Schéma typique :  

PC  
 │  
 │  BULK OUT  
 ▼  
USB → KS959 → IR LED  
                 ↓  
             air  
                 ↑  
USB ← KS959 ← IR photodiode  
 ▲  
 │ BULK IN  
 │  
PC
  
## Ce qui circule probablement dans ces endpoints  
Les trames IrDA ne sont généralement pas envoyées directement.  
La clé encapsule souvent :  
	[COMMAND][LEN][DATA][CHECKSUM]  
exemple :  
	01 05 12 34 56 78 9A  
où :  
	01  = SEND_FRAME  
	05  = length  
	data = IR payload  
et pour recevoir :  
	82 04 11 22 33 44  

## Séquence d'initialisation probable  
Quand le driver démarre, il fait généralement :  

- CONTROL TRANSFER  
- SET_MODE  
- CONTROL TRANSFER  
- SET_SPEED  
- CONTROL TRANSFER  
- ENABLE_RX  
par exemple :  
- SET_BAUD 9600  
- SET_BAUD 115200  
-SET_BAUD 4Mbps (IrDA SIR/FIR)  

#🚀 Le truc vraiment intéressant  
Si on comprend le protocole KS959, on peut faire beaucoup plus que l'ancien driver.  

Par exemple :  
- sniffer IrDA  
- émuler IrDA  
- bridge TCP ↔ IrDA  
- passerelle ESP32  
