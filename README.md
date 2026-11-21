Histoire :
=========
Ce projet est né d'une insatisfaction concernant l'authentification via capteur d'empreinte sous Plasma/KDE ainsi que l'utilisation en mode découverte de Mistral AI comme agent de code.
L'authentification est basée sur SDDM qui gère la fenêtre d'accueil où est choisi l'utilisateur et lui est demandé de s'authentifier et de PAM qui est la bibliothèque validant ou non les informations de sécurité envoyées.
Il y a donc un dialogue qui est engagé entre SDDM et PAM.
La frustration provient des points suivants :
1) il faut faire entrée pour que l'empreinte soit prise en compte
2) il y a un timeout de la cession de reconnaissance d'empreinte
3) le choix du type d'authentification est séquentiel alors qu'il devrait être possible en parallèle

Objet du projet
===============
Modifier la façon dont l'authentification est gérée afin que le choix du type d'authentification soit transparent.
L'utilisateur doit pouvoir commencer à taper son mot de passe mais se souvenir qu'il a la biométrie disponible sur ce PC et continuer avec cette dernière possibilité.
Inversement, se rendre compte que le capteur d'empreinte ne fonctionne pas bien (capteur sale par exemple) et continuer en choisissant de taper son mot de passe.

Ce projet est aussi pour moi l'opportunité d'apprendre à utiliser de façon intelligente l'aide agentique IA afin de compenser un manque d'agilité dans le code ainsi qu'un manque de temps à consacrer aux projets open source, bien que ce soit pourtant une envie très ancienne et correspondant profondément à mes principes et valeurs.
L'utilisation de l'IA je l'espère permettra à des personnes qui comme moi on une formation sérieuse en informatique (modélisation, langage, connaissance technique théoriques) mais n'ont pas l'expérience suffisante ou la possibilité de consacrer le temps nécessaire pour payer le ticket d'entrée qui est très élevé en ce domaine.
