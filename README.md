# KS959 open driver
![License](https://img.shields.io/github/license/pmariett/ks959-open-driver)
![GitHub repo size](https://img.shields.io/github/repo-size/pmariett/ks959-open-driver)
![GitHub stars](https://img.shields.io/github/stars/pmariett/ks959-open-driver?style=social)

### # Overview
La clé KS-959 est très répandue.
Elle était vendue sous plusieurs marques :
- Kingsun  
- Extended Systems  
- SigmaTel  
- clones OEM  

Mais toujours avec :
- VID_07D0  
- PID_4959  

---

### ## Repository structure
  
ks959-open-driver  
│  
├─ driver/  
│   └─ ks959_winusb.inf  
│  
├─ libks959/  
│   ├─ ks959.c  
│   ├─ ks959.h  
│   ├─ ks959_proto.c  
│   ├─ ks959_proto.h  
│   └─ ks959_device.c  
│  
├─ tools/  
│   ├─ ks959-monitor  
│   ├─ ks959-terminal  
│   └─ ks959-dump  
│  
└─ docs/  
    ├─ usb-protocol.md  
    ├─ some_stories.md  
    ├─ usb-architecture.md  
    ├─ ks959-protocol-model.md 	 
    ├─ ks959-protocol-notes.md  
    ├─ reverse-engineering.md  
    └─ hardware-notes.md  
 

---
