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

Difficulté du projet :
====================
la problématique principale est que la construction du dialogue entre SDDM et PAM se fasse via une ouverture de cession qui est séquentielle.
Cela touche à l'architecture même du processus plasma d'authentification.
J'ai pris les deux orientation alternatives et même complémentaires à terme suivantes :
1) raccourcir la séquence de reconnaissance d'empreinte qui et outrageusement grande actuellement (30s) et proposer un cycle rapide en pointillé d'ouverture de séquence de reconnaissance d'empreinte que quelques millisecondes (avec un paramétrage de calibrage) et proposer un délai de relance de la séquence d'empreinte via un délai rechargeable tant que l'utilisateur tape quelque chose dans la zone "mot de passe".
2) Lancer deux PAM concurrents :
   - l'un écoutant le capteur
   - l'autre écoutant le clavier
le premier des deux PAM ouverts qui réussit ouvre la cession plasma.

3) optionnel à valider par la pratique si c'est intéressant:
intégration proprement dans SDDM de façon que n'importe quel thème construit dessus pourra réutiliser de façon transparente les améliorations :
   - usage d'un écran d'accueil qui change selon le type d'authentification utilisé
   - utilisation d'un logo empreinte ou clavier pour signifier à l'utilisateur ce qui est actuellement écouté

Ces orientations sont les orientations actuelles elle seront validé par la pratique avant d'être publiées.
