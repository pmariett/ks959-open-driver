# Un peu d'histoire

## Pourquoi le driver 32 bits n'a jamais été porté  

La clé KS-959 date d’environ 2002-2005.  
À cette époque :  
- Windows XP  
- Windows 2000  
architecture x86 uniquement  

Les drivers étaient écrits avec :  
	Windows Driver Model (WDM)  

Problème :  
Ces drivers dépendaient souvent directement de :  
- irda.sys  
- irdastack.sys  
- ndis miniport  

La pile IrDA Windows ressemblait à ça :  
Application  
   │  
IrDA stack  
   │  
IrCOMM  
   │  
IrLAP  
   │  
Miniport driver  
   │  
USB driver  

##  Ce que Microsoft a fait ensuite  
Avec :  
- Windows 8  
- Windows 10  
Microsoft a abandonné la pile IrDA historique.  

### Conséquences :  
1 les miniports IrDA ne sont plus maintenus  
2 la documentation a disparu  
3 les API ont été retirées  

Donc porter le driver nécessiterait :  
réécrire toute la couche miniport  

## Et il y a un second problème  

Les vieux drivers utilisaient souvent :  
- URB  
- IOCTL internes  
- structures kernel obsolètes  

Exemple :  
	IRP_MJ_INTERNAL_DEVICE_CONTROL  

Ces structures ont changé.  
Donc un simple **recompile x64 ne marche pas**.  

## 	Troisième problème : signature  
Depuis Windows 10 :  
un driver kernel doit être :  
signé EV  
+ validé Microsoft  
Ce qui coûte :  
certificat EV ≈ 500 €/an  
Pour un produit mort commercialement, aucune entreprise ne va payer ça.  

##  Conclusion  
La clé KS-959 n’a jamais eu de driver 64 bits car :  
1 pile IrDA Windows abandonnée  
2 driver WDM obsolète  
3 coût de signature  
4 produit abandonné  

⭐ MAIS ce projet contourne tout ça  

L'approche :  
	WinUSB + lib utilisateur  

Architecture :  

Application  
   │  
libks959  
   │  
WinUSB  
   │  
USB  
   │  
KS959  
   │  
IR  

Donc :  
- pas de miniport  
- pas de kernel driver  
- pas de stack IrDA Windows  
👉 solution moderne  
