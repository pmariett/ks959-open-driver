# UWATEC Smart Protocol Notes

## Source
https://scratchpad.fandom.com/wiki/Uwatec_Smart_Protocol

## Résumé

Le site décrit le protocole de communication des ordinateurs Uwatec Smart / Aladin via IrDA et explique comment des développeurs ont réussi à communiquer avec ces ordinateurs pour télécharger les plongées.

Il précise dès le début que les informations sont issues d’un reverse-engineering et peuvent être incomplètes ou incorrectes.

Donc c’est exactement le même type de travail fait actuellement sur ce dépot, mais côté protocole applicatif, pas côté interface USB.

## Conséquence

Les conséquences directement exploitables sur mon projet :
Ma clé KS-959 fonctionne au niveau USB, mais le RX reste vide

Donc deux possibilités :
1 la clé n’est pas correctement initialisée
2 l’Aladin n’envoie rien tant qu’un dialogue protocolaire correct n’est pas initié

Et ce site suggère clairement la seconde hypothèse.
Les ordinateurs Uwatec utilisent une communication command/response, pas un simple flux passif.
Par exemple :
il existe une séquence de handshake
puis une commande version
puis différentes commandes pour lire les données.

Donc le PC doit parler en premier.
Mon RX reste vide car je n’ai jamais envoyé de commande valide pour l’Aladin.

Donc le comportement observé est cohérent :

- le dongle attend un flux IR
- l’Aladin attend une commande
- personne ne parle → silence total

Ce que le site suggère implicitement c'est que la communication ressemble à ceci :

PC → handshake
Aladin → réponse
PC → version request
Aladin → version
PC → read dive list
Aladin → data

Donc sans commande IR initiale, l’Aladin reste muet.

Cela change légèrement l’objectif :

Jusqu’ici on cherchait comment lire un flux IR passif, maais la vraie question est probablement : comment émettre une première commande IrDA valide vers l’Aladin.

## Ce qu'on comprend du site
Le protocole semble utiliser :
- des trames structurées
- avec checksum
- et parfois un handshake pseudo-session.


