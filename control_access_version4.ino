/***************************************************
  Gestion des Empreintes Digitales avec ESP32
****************************************************/
#include <Arduino.h>
#include <esp_system.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "LittleFS.h"
#include <Arduino_JSON.h>
#include <Adafruit_Fingerprint.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include "time.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <vector>
#include <algorithm>
#include <set>  // Pour std::set, utilisé dans la synchronisation des IDs


//les constante pour gerer les erreurs pendant l'enregistrement:
#define FINGERPRINT_MUTEX_TIMEOUT -10
#define FINGERPRINT_NOT_CONNECTED -11
#define FINGERPRINT_TIMEOUT_1 -12
#define FINGERPRINT_TIMEOUT_2 -13
#define FINGERPRINT_TIMEOUT_3 -14
#define FINGERPRINT_DUPLICATE -15
#define FINGERPRINT_BLOQUE -16
#define FINGERPRINT_UNKNOWN_ERROR -99  // Erreur inconnue

//constante pour la gestion des checkIn/checkOut
#define MAX_ESSAIS 3
#define DELAI_BLOQUAGE 30000
#define LED_BUILTIN 2  // Change à 4, 5, 16 si ta LED ne s’allume pas

#define MAX_COMMANDES 10

// Gestion des essais de vérification
int essaisRestants = MAX_ESSAIS;
unsigned long tempsDebutBlocage = 0;
bool verificationBloquee = false;

////////////////// Configuration Wi-Fi - nouvelle fonctionnalite pour gerer la connection/////////////////////
// Configuration des réseaux par défaut
#define MAX_WIFI_ATTEMPTS 1
#define WIFI_ATTEMPTS_FILE "/conteurWifi"
#define WIFI_CONF_FILE "/wifi.conf"
const char* DEFAULT_SSIDS[] = { "DESKTOP-6MDLUNL", "iPhone" };
const char* DEFAULT_PASSWORDS[] = { "maxima123", "toto2040" };
const int MAX_DEFAULT_NETWORKS = sizeof(DEFAULT_SSIDS) / sizeof(DEFAULT_SSIDS[0]);

volatile bool wifiConfigSuccess = false;
unsigned long wifiConfigSuccessMillis = 0;

// État persistant entre les redémarrages
int wifiConnectionAttempts = 0;
bool configPortalActive = false;
unsigned long portalStartTime = 0;
const unsigned long CONFIG_PORTAL_TIMEOUT = 5 * 60 * 1000;  // 5 minutes

// Identifiants configurés
String configuredSSID = "";
String configuredPassword = "";

// Serveur Web
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Serveur web pour le portail de configuration
AsyncWebServer configServer(80);

// Capteur d'empreintes
#define FINGERPRINT_RX_PIN 16
#define FINGERPRINT_TX_PIN 17
HardwareSerial busUARTpourCapteurEmpreinte(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&busUARTpourCapteurEmpreinte);

// Écran LCD
#define LCD_SDA 21
#define LCD_SCL 22
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Variables globales pour la gestion de l'écran LCD
String ligne1Lcd = "";
String ligne2Lcd = "";
unsigned long lcdMessageEndTime = 0;
bool lcdClearAfter = false;
unsigned long lastLcdUpdateTime = 0;              // Pour contrôler la fréquence de rafraîchissement de l'affichage IP/Heure
const unsigned long LCD_UPDATE_INTERVAL = 10000;  // Intervalle de rafraîchissement (10 secondes)


// Configuration NTP
const char* ntpServer = "pool.ntp.org";  //serveur qui va nous fournir l'heure
const long gmtOffset_sec = 3600 * 1;     // GMT+1 pour la ville de Douala
const int daylightOffset_sec = 0;        // Pas d'heure d'été puisque nous somme en afrique centrale

//variables globales de la verification de la date du jour
String currentDate = "";
unsigned long lastDateCheck = 0;
const unsigned long DATE_CHECK_INTERVAL = 60000;  // Vérifier toutes les minutes

// Mutex pour protéger l'accès au capteur d'empreintes
SemaphoreHandle_t xMutexCapteurEmpreinte;

struct InformationsEmploye {
  uint8_t id;
  String nom;
  String checkIn = "";         // Heure d'arrivée
  String checkOut = "";        // Heure de départ
  String dateConnection = "";  // Date du jour (YYYY-MM-DD)
};

// Liste des informations complètes des utilisateurs/employés
std::vector<InformationsEmploye> listeDesUtilisateur;
SemaphoreHandle_t xMutexListeUtilisateurEnregistre;  // Le mutex protège maintenant cette nouvelle structure
const char* FICHIER_EMPLOYE = "/employes.json";      // Nom du fichier pour la sauvegarde/lecture

SemaphoreHandle_t xMutexLCD;
SemaphoreHandle_t xMutexFlags;


// Structure pour les commandes WebSocket
struct commandeWebSocket {
  AsyncWebSocketClient* client;
  String command;
  JSONVar messageJsonRecu;
};

//Protection des accès WebSocket avec mutex
SemaphoreHandle_t xMutexWebSocket;

// Pool statique de commandes
commandeWebSocket commandePool[MAX_COMMANDES];
int indexCommande = 0;

// Structure pour résultats de vérification
struct ResultatVerification {
  int code;
  int id;
  String message;
};

// Définition des états
enum EtatEmpreinte {
  ETAT_ATTENTE_DOIGT,
  ETAT_VERIFICATION_EN_COURS,
  ETAT_RECONNAISSANCE_REUSSIE,
  ETAT_ERREUR_RECONNAISSANCE,
  ETAT_SYSTEME_BLOQUE
};

// Structure de contexte
struct ContexteVerification {
  EtatEmpreinte etatActuel;
  int essaisRestants;
  unsigned long chronoDebut;
  int idUtilisateur;
  ResultatVerification dernierResultat;
};


// File d'attente pour les commandes
//les handles freeRTOS sont des references (alias) vers des variables crees par le systeme
QueueHandle_t xfileDattenteDesCommandeWebsockets = NULL;

// Flag pour contrôler la tâche d'identification
//volatile ici c'est pour garantir que toute les taches du syteme soient au courant des modifications de la variable
volatile bool tacheDeLectureEmpreinteEnabled = true;
volatile bool operationsCritiquesEnCours = false;
volatile bool tacheSyncNTPEnabled = true;
volatile bool tacheStatutSystemeEnabled = true;

// Prototype des fonctions
void initWiFi();
void launchConfigPortal();
void initWiFiWithFallback();
void handleConfigPortal();
void loadWiFiCredentials();
void saveWiFiCredentials();
bool connectToWiFi(const char* ssid, const char* password, unsigned long timeout = 15000);
void saveWiFiCredentials();
void loadAttempts();
void resetAttempts();

void initLittleFS();
void initWebSocket();
void onEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len);
void notifyClients(const String message);
String getFormattedTime();
void messageSurLcd(String line1, String line2, unsigned long duration_ms, bool clearFirst);
void initNTP();
String laBonneHeure();
void effaceLigne(uint8_t positionColonne, uint8_t positionLigne);
void efface();

//fonction pour la gestion de la liste des employes
void sauvegardeInformationUtilisateur();
void lectureInformationsUtilisateur();
std::set<uint8_t> getIDsSurCapteur();
void synchroniserListeEmployesAvecCapteur();
void ajouterEmployeAListe(uint8_t id, const String& nom);
void supprimerEmployeDeListe(uint8_t id);


// Fonctions du capteur d'empreintes
int enregistrementEmpreinte(uint8_t id, const String& nom, String& messageErreur);
int suppressionEmpreinte(uint8_t id, String& messageErreur);
void verifierEmpreinteDetaillee(ResultatVerification& resultat);
void envoiDeLaListeUtilisateurEnregistre(AsyncWebSocketClient* client);

// Tâches FreeRTOS(le prefixe pv de pvParameters signifie pointer void )
void tacheDeLectureEmpreinte(void* pvParameters);
void tacheDetraitementDesCommandesWebSockets(void* pvParameters);
void tacheSyncNTP(void* pvParameters);
void tacheStatutSysteme(void* pvParameters);

// ========== les pages web de mon mode point d'acees esp32 ==========

const char wifiConfigPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Configuration WiFi - Système Empreinte</title>
    <style>
        :root {
            --primary: #0A1128;
            --secondary: #034078;
            --accent: #2A9D8F;
            --light: #f5f7fa;
            --gray: #6c757d;
            --success: #28a745;
            --warning: #ffc107;
            --danger: #dc3545;
        }
        
        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }
        
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #f5f7fa 0%, #e4e7f1 100%);
            color: #333;
            line-height: 1.6;
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            padding: 20px;
        }
        
        .header {
            text-align: center;
            padding: 30px 0;
            margin-bottom: 30px;
        }
        
        .header h1 {
            color: var(--primary);
            font-size: 2.5rem;
            margin-bottom: 10px;
            font-weight: 700;
        }
        
        .header p {
            color: var(--gray);
            font-size: 1.1rem;
            max-width: 600px;
            margin: 0 auto;
        }
        
        .device-info {
            background-color: white;
            border-radius: 15px;
            box-shadow: 0 6px 20px rgba(0, 0, 0, 0.08);
            padding: 25px;
            margin-bottom: 30px;
            text-align: center;
        }
        
        .device-id {
            display: inline-block;
            background: var(--primary);
            color: white;
            padding: 8px 20px;
            border-radius: 50px;
            font-weight: 600;
            margin-top: 15px;
            font-size: 0.9rem;
        }
        
        .card {
            background-color: white;
            border-radius: 15px;
            box-shadow: 0 6px 20px rgba(0, 0, 0, 0.08);
            overflow: hidden;
            max-width: 500px;
            margin: 0 auto 40px;
            width: 100%;
        }
        
        .card-header {
            background: linear-gradient(135deg, var(--primary) 0%, var(--secondary) 100%);
            color: white;
            padding: 20px;
            text-align: center;
        }
        
        .card-header h2 {
            font-size: 1.8rem;
            font-weight: 600;
        }
        
        .card-body {
            padding: 30px;
        }
        
        .form-group {
            margin-bottom: 25px;
        }
        
        label {
            display: block;
            margin-bottom: 8px;
            font-weight: 600;
            color: var(--primary);
            font-size: 1.05rem;
        }
        
        input[type="text"],
        input[type="password"] {
            width: 100%;
            padding: 14px 18px;
            border: 2px solid #e1e5eb;
            border-radius: 10px;
            font-size: 1rem;
            transition: all 0.3s ease;
        }
        
        input[type="text"]:focus,
        input[type="password"]:focus {
            border-color: var(--accent);
            outline: none;
            box-shadow: 0 0 0 3px rgba(42, 157, 143, 0.2);
        }
        
        .btn-submit {
            display: block;
            width: 100%;
            padding: 16px;
            background: linear-gradient(135deg, var(--accent) 0%, #1d7a6b 100%);
            color: white;
            border: none;
            border-radius: 10px;
            font-size: 1.1rem;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s ease;
            box-shadow: 0 4px 10px rgba(42, 157, 143, 0.3);
        }
        
        .btn-submit:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 15px rgba(42, 157, 143, 0.4);
        }
        
        .btn-submit:active {
            transform: translateY(0);
        }
        
        .footer {
            text-align: center;
            padding: 20px;
            color: var(--gray);
            font-size: 0.9rem;
            margin-top: auto;
        }
        
        .status-bar {
            display: flex;
            justify-content: center;
            gap: 20px;
            margin-top: 15px;
            flex-wrap: wrap;
        }
        
        .status-item {
            display: flex;
            align-items: center;
            background: rgba(10, 17, 40, 0.08);
            padding: 8px 15px;
            border-radius: 50px;
            font-size: 0.85rem;
        }
        
        .status-item svg {
            margin-right: 8px;
            width: 18px;
            height: 18px;
        }
        
        @media (max-width: 600px) {
            .card-body {
                padding: 20px;
            }
            
            .header h1 {
                font-size: 2rem;
            }
        }
        
        .scanning-animation {
            display: flex;
            justify-content: center;
            margin: 20px 0;
        }
        
        .dot {
            width: 12px;
            height: 12px;
            background-color: var(--accent);
            border-radius: 50%;
            margin: 0 5px;
            animation: pulse 1.5s infinite ease-in-out;
        }
        
        .dot:nth-child(2) {
            animation-delay: 0.2s;
        }
        
        .dot:nth-child(3) {
            animation-delay: 0.4s;
        }
        
        @keyframes pulse {
            0%, 100% { transform: scale(1); opacity: 1; }
            50% { transform: scale(1.2); opacity: 0.7; }
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>Configuration WiFi</h1>
        <p>Connectez votre système de pointage à votre réseau</p>
    </div>
    
    <div class="device-info">
        <h3>Système de Pointage Empreinte</h3>
        <div class="device-id">ID: ESP32-%CHIPID%</div>
    </div>
    
    <div class="card">
        <div class="card-header">
            <h2>Paramètres réseau</h2>
        </div>
        <div class="card-body">
            <form id="wifi-form">
                <div class="form-group">
                    <label for="ssid">Nom du réseau (SSID)</label>
                    <input type="text" id="ssid" name="ssid" required autofocus placeholder="Entrez le nom de votre réseau WiFi">
                </div>
                
                <div class="form-group">
                    <label for="password">Mot de passe WiFi</label>
                    <input type="password" id="password" name="password" placeholder="Entrez le mot de passe du réseau">
                </div>
                
                <button type="submit" class="btn-submit">Enregistrer et redémarrer</button>
            </form>
        </div>
    </div>
    
    <div class="scanning-animation">
        <div class="dot"></div>
        <div class="dot"></div>
        <div class="dot"></div>
    </div>
    
    <div class="status-bar">
        <div class="status-item">
            <svg viewBox="0 0 24 24" fill="currentColor">
                <path d="M4,6H20V16H4M20,18A2,2 0 0,0 22,16V6C22,4.89 21.1,4 20,4H4C2.89,4 2,4.89 2,6V16A2,2 0 0,0 4,18H0V20H24V18H20Z"/>
            </svg>
            <span>Mode Configuration</span>
        </div>
        <div class="status-item">
            <svg viewBox="0 0 24 24" fill="currentColor">
                <path d="M12,20A8,8 0 0,0 20,12A8,8 0 0,0 12,4A8,8 0 0,0 4,12A8,8 0 0,0 12,20M12,2A10,10 0 0,1 22,12A10,10 0 0,1 12,22C6.47,22 2,17.5 2,12A10,10 0 0,1 12,2M12.5,7V12.25L17,14.92L16.25,16.15L11,13V7H12.5Z"/>
            </svg>
            <span>%TIMEOUT%</span>
        </div>
    </div>
    
    <div class="footer">
        <p>Système de Pointage par Empreinte Digitale &copy; 2023</p>
    </div>
    
    <script>
        document.addEventListener('DOMContentLoaded', function() {
            // Mettre à jour l'ID du périphérique
            const chipId = '%CHIPID%';
            document.querySelector('.device-id').textContent = `ID: ESP32-${chipId}`;
            
            // Mettre à jour le compte à rebours
            let timeout = 5 * 60; // 5 minutes en secondes
            const timeoutElement = document.querySelector('.status-item:nth-child(2) span');
            
            function updateTimer() {
                const minutes = Math.floor(timeout / 60);
                const seconds = timeout % 60;
                timeoutElement.textContent = `Redémarrage dans ${minutes}:${seconds < 10 ? '0' : ''}${seconds}`;
                
                if (timeout <= 0) {
                    document.body.innerHTML = '<div style="text-align:center;padding:50px"><h2>Temps écoulé! Redémarrage en cours...</h2></div>';
                    return;
                }
                
                timeout--;
                setTimeout(updateTimer, 1000);
            }
            
            updateTimer();
            
            // Gestion du formulaire
            const form = document.getElementById('wifi-form');
            form.addEventListener('submit', function(e) {
                e.preventDefault();
                
                const ssid = document.getElementById('ssid').value;
                const password = document.getElementById('password').value;
                
                if (!ssid) {
                    alert('Veuillez entrer un nom de réseau WiFi');
                    return;
                }
                
                // Afficher un indicateur de chargement
                const button = form.querySelector('button');
                button.innerHTML = '<div class="scanning-animation" style="justify-content:center;margin:0"><div class="dot"></div><div class="dot"></div><div class="dot"></div></div>';
                button.disabled = true;
                
                // Envoyer les données au serveur
                fetch('/configure', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                    },
                    body: `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`
                })
                .then(response => {
                    if (response.ok) {
                        button.innerHTML = 'Configuration réussie!';
                        button.style.background = 'linear-gradient(135deg, #28a745 0%, #1e7e34 100%)';
                        
                        // Afficher un message de succès
                        document.body.innerHTML = `
                            <div style="text-align:center;padding:50px">
                                <h2 style="color:#28a745">Configuration réussie!</h2>
                                <p>Votre système va redémarrer et tenter de se connecter au réseau.</p>
                                <p>Si la connexion échoue, vous pourrez accéder à nouveau à cette page.</p>
                            </div>
                        `;
                    } else {
                        throw new Error('Erreur lors de la configuration');
                    }
                })
                .catch(error => {
                    console.error('Error:', error);
                    button.innerHTML = 'Erreur - Réessayer';
                    button.disabled = false;
                    alert('Une erreur est survenue: ' + error.message);
                });
            });
        });
    </script>
</body>
</html>
)rawliteral";

const char successPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Configuration Réussie</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #f5f7fa 0%, #e4e7f1 100%);
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            text-align: center;
            padding: 20px;
        }
        
        .success-card {
            background: white;
            border-radius: 15px;
            box-shadow: 0 10px 30px rgba(0, 0, 0, 0.1);
            padding: 40px;
            max-width: 500px;
            width: 100%;
        }
        
        .success-icon {
            width: 80px;
            height: 80px;
            background: #28a745;
            border-radius: 50%;
            display: flex;
            align-items: center;
            justify-content: center;
            margin: 0 auto 20px;
        }
        
        .success-icon svg {
            width: 40px;
            height: 40px;
            fill: white;
        }
        
        h1 {
            color: #28a745;
            margin-bottom: 15px;
            font-size: 2.2rem;
        }
        
        p {
            color: #6c757d;
            margin-bottom: 25px;
            font-size: 1.1rem;
            line-height: 1.6;
        }
        
        .device-info {
            background: #f8f9fa;
            border-radius: 10px;
            padding: 15px;
            margin: 20px 0;
            font-size: 0.9rem;
        }
        
        .progress-bar {
            height: 8px;
            background: #e9ecef;
            border-radius: 4px;
            overflow: hidden;
            margin-top: 30px;
        }
        
        .progress {
            height: 100%;
            background: #28a745;
            width: 100%;
            animation: progressAnimation 5s linear forwards;
        }
        
        @keyframes progressAnimation {
            0% { width: 100%; }
            100% { width: 0%; }
        }
    </style>
</head>
<body>
    <div class="success-card">
        <div class="success-icon">
            <svg viewBox="0 0 24 24">
                <path d="M9,20.42L2.79,14.21L5.62,11.38L9,14.77L18.88,4.88L21.71,7.71L9,20.42Z"/>
            </svg>
        </div>
        
        <h1>Configuration Réussie!</h1>
        <p>Votre système va maintenant redémarrer et tenter de se connecter au réseau WiFi:</p>
        
        <div class="device-info">
            <strong>%SSID%</strong>
        </div>
        
        <p>Si la connexion échoue, vous pourrez accéder à nouveau à cette page en vous connectant au point d'accès du système.</p>
        
        <div class="progress-bar">
            <div class="progress"></div>
        </div>
    </div>
</body>
</html>
)rawliteral";



void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  Serial.printf("\n\n===== Boot v3.0 (CPU %d MHz) =====\n", ESP.getCpuFreqMHz());
  Serial.printf("Heap libre: %d bytes\n", ESP.getFreeHeap());

  // Initialisation des mutex(mutual exclusion)
  //le prefixe x est une convention qui permet de signaler que la fonction freeRTOS renvoi un handle
  xMutexCapteurEmpreinte = xSemaphoreCreateMutex();
  if (xMutexCapteurEmpreinte == NULL) {
    Serial.println("Erreur: Impossible de creer le mutex de protection des accès WebSocket. Redemarrage requis.");
    ESP.restart();
  }

  xMutexWebSocket = xSemaphoreCreateMutex();
  if (xMutexWebSocket == NULL) {
    Serial.println("Erreur: Impossible de creer le mutex d. Redemarrage requis.");
    ESP.restart();
  }

  xMutexListeUtilisateurEnregistre = xSemaphoreCreateMutex();
  if (xMutexListeUtilisateurEnregistre == NULL) {
    Serial.println("Erreur: Impossible de creer le mutex pour la liste des IDs");
    ESP.restart();
  }

  xMutexLCD = xSemaphoreCreateMutex();
  if (xMutexLCD == NULL) {
    Serial.println("Erreur: Impossible de creer le mutex pour le LCD");
    ESP.restart();
  }

  xMutexFlags = xSemaphoreCreateMutex();
  if (xMutexFlags == NULL) {
    Serial.println("Erreur: Impossible de creer le mutex pour les flags");
    ESP.restart();
  }

  // Initialisation de la file de commandes qui va stocker les commandes webSocket
  xfileDattenteDesCommandeWebsockets = xQueueCreate(10, sizeof(commandeWebSocket*));
  if (xfileDattenteDesCommandeWebsockets == NULL) {
    Serial.println("Erreur: Impossible de creer la file de commandes");
    ESP.restart();
  }


  // Initialisation du LCD
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  messageSurLcd("Demarrage ESP32", "Chargement...", 3000, true);

  // Initialisation du bus de communication avec le capteur d'empreintes
  busUARTpourCapteurEmpreinte.begin(57600, SERIAL_8N1, FINGERPRINT_RX_PIN, FINGERPRINT_TX_PIN);
  vTaskDelay(pdMS_TO_TICKS(100));

  if (!finger.verifyPassword()) {
    Serial.println("Capteur non trouve. Verifiez le cablage!");
    messageSurLcd("Capteur FP", "Non trouve!", 5000, true);

    for (int i = 0; i < 5; i++) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      if (finger.verifyPassword()) {
        Serial.println("Capteur finalement trouve!");
        break;
      }
    }
    if (!finger.verifyPassword()) {
      while (1) {                        //on bloque le programme si on n'a pas trouvez le lecteur d'empreinte
                                         //pdMS_TO_TICKS est une macro qui converti une valeur de temps en millisecondes en ticks
                                         //un ticks est l'unite de temps utilisée par freeRTOS pour mesurer le temps
        vTaskDelay(pdMS_TO_TICKS(100));  //le prefixe v indique que la fonction ne renvoie rien
      }
    }
  }
  Serial.println("Capteur d'empreintes trouve!");

  initLittleFS();
  loadWiFiCredentials();
  if (configuredSSID == "") {
    launchConfigPortal();
    // Boucle d'attente jusqu'à config terminée
    while (configPortalActive) {
      handleConfigPortal();
      delay(10);
    }
    // Reboot automatique après config et sauvegarde
    return;
  }
  // Ici, on a des identifiants, donc on tente la connexion STA
  initWiFiWithFallback();
  //initWiFi();

  // Si on est en mode configuration, ne pas initialiser le reste du système
  if (configPortalActive) {
    return;  // Le reste de l'initialisation se fera après la configuration
  }



  initNTP();                                          // Initialisation NTP après la connexion WiFi
  currentDate = getFormattedTime().substring(0, 10);  // "YYYY-MM-DD"
  initWebSocket();

  //gestion de la liste des employees
  lectureInformationsUtilisateur();        // Charge la liste depuis le fichier
  synchroniserListeEmployesAvecCapteur();  // Synchronisation de la liste en memoire avec celle du capteur réel
  // Configuration et démarrage du serveur web
  //server.serveStatic("/", LittleFS, "/");
  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Page non trouvee");
  });
  server.begin();

  // Créer les tâches FreeRTOS
  xTaskCreatePinnedToCore(
    tacheDeLectureEmpreinte,
    "tacheDeLectureEmpreinte",
    8192,
    NULL,
    3,  // Priorité basse
    NULL,
    0  // Core 0
  );

  xTaskCreatePinnedToCore(
    tacheDetraitementDesCommandesWebSockets,
    "commandeEmisePourLaFileDattenteProcessing",
    12288,
    NULL,
    6,  // Priorité plus élevée pour les commandes
    NULL,
    1  // Core 1
  );

  xTaskCreatePinnedToCore(
    tacheSyncNTP,
    "tacheSyncNTP",
    4096,
    NULL,
    2,
    NULL,
    0);

  xTaskCreatePinnedToCore(
    tacheStatutSysteme,
    "tacheStatutSysteme",
    8192,
    NULL,
    2,
    NULL,
    1);

  messageSurLcd("Systeme pret!", "Connectez-vous", 3000, true);
}


void loop() {

  // Gestion du portail de configuration
  if (configPortalActive) {
    handleConfigPortal();
    delay(10);
    if (!wifiConfigSuccess) return;  // Ne pas exécuter le reste du code en mode config
  }
  if (wifiConfigSuccess && millis() - wifiConfigSuccessMillis > 1000) {
    configServer.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    wifiConnectionAttempts = 0;
    configPortalActive = false;
    ESP.restart();
  }

  // Si des opérations critiques (comme l'enregistrement d'empreintes) sont en cours,
  // on ne fait rien dans la loop pour éviter les interférences.
  if (operationsCritiquesEnCours) {
    vTaskDelay(pdMS_TO_TICKS(100));
    return;
  }


  // Vérification périodique de la connexion WiFi
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) {  // Toutes les 30s
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Perte de connexion WiFi!");
      wifiConnectionAttempts++;
      if (wifiConnectionAttempts >= 2) {
        launchConfigPortal();
      }
    }
    lastWifiCheck = millis();
  }

  // --- GESTION DE L'AFFICHAGE TEMPORAIRE DU LCD ---
  // Vérification si un message temporaire a expiré
  if (lcdMessageEndTime != 0 && millis() >= lcdMessageEndTime) {
    if (xSemaphoreTake(xMutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (lcdClearAfter) {
        lcd.clear();  // Efface l'écran si demandé par le message temporaire
      }
      // Réinitialise les drapeaux du message temporaire
      lcdMessageEndTime = 0;
      ligne1Lcd = "";  // Optionnel : réinitialiser les contenus
      ligne2Lcd = "";
      lcdClearAfter = false;
      xSemaphoreGive(xMutexLCD);
    } else {
      Serial.println("Warning: Impossible d'acquérir le mutex LCD pour effacer le message expiré.");
    }
    lastLcdUpdateTime = 0;  // Force une mise à jour immédiate de l'IP/Heure
  }

  // --- AFFICHAGE IP / HEURE (seulement si aucun message temporaire n'est actif) ---.
  // On affiche l'IP/Heure seulement si aucun message temporaire n'est en cours (lcdMessageEndTime == 0)
  // et si l'intervalle est atteint.
  // --- AFFICHAGE ALTERNÉ (IP/Heure et Système prêt) ---
  static bool displaySystemReady = false;
  static unsigned long lastSwitchTime = 0;
  const unsigned long DISPLAY_SWITCH_INTERVAL = 3000;  // 3s par écran

  if (lcdMessageEndTime == 0 && millis() - lastLcdUpdateTime >= LCD_UPDATE_INTERVAL) {
    // Alterner entre les deux écrans toutes les 3 secondes
    if (millis() - lastSwitchTime >= DISPLAY_SWITCH_INTERVAL) {
      displaySystemReady = !displaySystemReady;
      lastSwitchTime = millis();
    }

    String line1, line2;

    if (displaySystemReady) {
      line1 = "Systeme pret";
      line2 = "Placez votre doigt";
    } else {
      line1 = WiFi.localIP().toString() + "/";
      line2 = "Heure: " + laBonneHeure();
    }

    if (xSemaphoreTake(xMutexLCD, pdMS_TO_TICKS(1000)) == pdTRUE) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(line1);
      lcd.setCursor(0, 1);
      lcd.print(line2);
      xSemaphoreGive(xMutexLCD);
    } else {
      Serial.println("Warning: Mutex LCD indisponible");
    }
    lastLcdUpdateTime = millis();
  }

  vTaskDelay(pdMS_TO_TICKS(1000));  // Pause de 1 seconde pour la tâche loop
}

void initWiFi() {

  // Étape 1: Chargement des identifiants WiFi
  loadWiFiCredentials();

  // Étape 2: Initialissation de la connexion WiFi
  initWiFiWithFallback();

  //reedemarage de la connexion automatique
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == WIFI_EVENT_STA_DISCONNECTED) {
      Serial.println("Reconnexion WiFi...");
      WiFi.reconnect();
    }
  });

  Serial.println("Connecté au WiFi!");
  Serial.print("Adresse IP: ");
  Serial.println(WiFi.localIP());
}

void launchConfigPortal() {
  Serial.println("Lancement du portail de configuration WiFi");
  configPortalActive = true;
  portalStartTime = millis();

  // Créer un SSID unique basé sur l'ID du chip
  uint32_t chipId = ESP.getEfuseMac();
  char apSSID[30];
  snprintf(apSSID, sizeof(apSSID), "ESP32-Config-%04X", (uint16_t)(chipId >> 32));

  // Démarrer le point d'accès
  WiFi.disconnect(true);  // Déconnecte toute connexion existante
  WiFi.mode(WIFI_AP);     // Passe en mode AP UNIQUEMENT
  delay(100);             // Petit délai pour la stabilité
  WiFi.softAP(apSSID);
  IPAddress apIP = WiFi.softAPIP();

  Serial.print("Point d'accès démarré: ");
  Serial.println(apSSID);
  Serial.print("Adresse IP: ");
  Serial.println(apIP);

  messageSurLcd("Mode Configuration", apSSID, 0, true);
  messageSurLcd("Allez sur", apIP.toString(), 0, false);

  // Configurer les routes du serveur web
  configServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = FPSTR(wifiConfigPage);
    html.replace("%CHIPID%", String((uint16_t)(ESP.getEfuseMac() >> 32), HEX));
    html.replace("%TIMEOUT%", "Redémarrage dans 5:00");
    request->send(200, "text/html", html);
  });

  configServer.on("/configure", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
      configuredSSID = request->getParam("ssid", true)->value();
      configuredPassword = request->getParam("password", true)->value();

      // Sauvegarder les identifiants
      saveWiFiCredentials();

      // Envoyer la page de succès
      String html = FPSTR(successPage);
      html.replace("%SSID%", configuredSSID);
      request->send(200, "text/html", html);
      configPortalActive = false;
      wifiConfigSuccess = true;
      wifiConfigSuccessMillis = millis();

      messageSurLcd("Config reussie!", "Redemarrage...", 1000, true);
    } else {
      request->send(400, "text/plain", "Paramètres manquants");
    }
  });

  configServer.begin();
}

// --- Gestion persistante du compteur d'essais ---
void loadAttempts() {
  File f = LittleFS.open(WIFI_ATTEMPTS_FILE, "r");
  if (f) {
    wifiConnectionAttempts = f.parseInt();
    f.close();
  } else {
    wifiConnectionAttempts = 0;
  }
}

void saveAttempts() {
  File f = LittleFS.open(WIFI_ATTEMPTS_FILE, "w");
  if (f) {
    f.print(wifiConnectionAttempts);
    f.close();
  }
}

void resetAttempts() {
  wifiConnectionAttempts = 0;
  saveAttempts();
}

// ========== WIFI MANAGEMENT FUNCTIONS ==========

// Tenter de se connecter à un réseau WiFi
bool connectToWiFi(const char* ssid, const char* password, unsigned long timeout) {
  Serial.printf("Tentative de connexion à: %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startTime < timeout)) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnecté avec succès à %s\n", ssid);
    Serial.print("Adresse IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.printf("\nÉchec de connexion à %s\n", ssid);
  return false;
}

// Sauvegarder les identifiants WiFi dans LittleFS
void saveWiFiCredentials() {
  File file = LittleFS.open(WIFI_CONF_FILE, "w");
  if (file) {
    file.println(configuredSSID);
    file.println(configuredPassword);
    file.close();
    Serial.println("Identifiants WiFi sauvegardés");
    Serial.println("Contenu du fichier wifi.conf :");
  } else {
    Serial.println("Erreur: échec de la sauvegarde des identifiants WiFi");
  }
  file.close();
  File fileR = LittleFS.open("/wifi.conf", "r");
  if (fileR) {
    while (file.available()) {
      Serial.write(file.read());
    }
  }
  fileR.close();
}

// Charger les identifiants WiFi depuis LittleFS
void loadWiFiCredentials() {
  if (LittleFS.exists("/wifi.conf")) {
    File file = LittleFS.open(WIFI_CONF_FILE, "r");
    if (file && file.size() > 0) {  // Vérifier la taille du fichier
      configuredSSID = file.readStringUntil('\n');
      configuredPassword = file.readStringUntil('\n');
      Serial.println("Contenu du fichier wifi.conf :");
      while (file.available()) {
        Serial.write(file.read());
      }
      configuredSSID.trim();
      configuredPassword.trim();
    } else {
      if (file) file.close();
      launchConfigPortal();  // Fichier vide/corrompu
    }
  } else {
    launchConfigPortal();  // Fichier inexistant
  }
}

// Gestion du portail de configuration
void handleConfigPortal() {
  if (!configPortalActive) return;

  // Vérifier le timeout
  if (millis() - portalStartTime > CONFIG_PORTAL_TIMEOUT) {
    Serial.println("Timeout du portail de configuration");
    messageSurLcd("Timeout config", "Redemarrage...", 3000, true);
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP.restart();
  }
}

// Fonction principale de connexion WiFi
void initWiFiWithFallback() {
  // Éviter les reset WiFi agressifs
  static bool firstBoot = true;
  loadAttempts();

  if (firstBoot) {
    WiFi.persistent(false);
    WiFi.disconnect(false);  // Ne pas effacer les paramètres
    WiFi.mode(WIFI_STA);
    firstBoot = false;
  }
  // 1. Tenter d'abord le réseau configuré manuellement
  if (configuredSSID != "" && connectToWiFi(configuredSSID.c_str(), configuredPassword.c_str())) {
    Serial.println("Connecté au réseau configuré manuellement");
    wifiConnectionAttempts = 0;
    return;
  }


  // 2. Tenter les réseaux par défaut
  for (int i = 0; i < MAX_DEFAULT_NETWORKS; i++) {
    if (connectToWiFi(DEFAULT_SSIDS[i], DEFAULT_PASSWORDS[i])) {
      Serial.println("Connecté à un réseau par défaut");
      wifiConnectionAttempts = 0;
      return;
    }
  }

  // 3. Gestion des échecs
  wifiConnectionAttempts++;
  saveAttempts();
  Serial.printf("Échec de connexion. Tentative %d/2\n", wifiConnectionAttempts);

  if (wifiConnectionAttempts < 2) {
    messageSurLcd("Echec WiFi", "Redemarrage...", 3000, true);
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP.restart();
  }

  resetAttempts();
  // 4. Après 2 échecs, lancer le portail de configuration
  launchConfigPortal();
}


void initNTP() {
  Serial.println("Configuration NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {  // Timeout de 10 secondes
    Serial.println("Echec de la premiere synchronisation NTP!");
    // Réessayer avec un autre serveur
    configTime(gmtOffset_sec, daylightOffset_sec, "time.google.com", "time.windows.com");

    if (!getLocalTime(&timeinfo, 10000)) {
      Serial.println("Echec definitif de la synchronisation NTP!");
      JSONVar erreurNTP;
      erreurNTP["action"] = "ntpError";
      erreurNTP["message"] = "Echec synchronisation NTP";
      notifyClients(JSON.stringify(erreurNTP));
      return;
    }
  }

  char timeStr[64];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S le %Y-%m-%d ", &timeinfo);
  Serial.printf("Heure synchronisee: %s\n", timeStr);
  messageSurLcd("Heure NTP", timeStr, 3000, true);
}

/**
 * @brief Récupère l'heure locale actuelle (synchronisée via NTP)
 * et la retourne suivant le format HH:MM:SS.
 */
String laBonneHeure() {
  struct tm timeinfo;  // Structure pour stocker les composants de la date et de l'heure.

  // Tente de récupérer l'heure actuelle de l'horloge interne de l'ESP32 (déjà synchronisée par NTP).
  if (!getLocalTime(&timeinfo, 500)) {  // Timeout 500ms
    return "--:--:--";
  }

  // --- Affichage sur l'écran LCD (HH:MM:SS) ---
  char timeString[17];  // Tableau de caractères pour stocker la chaîne de temps (16 chars + null terminator).

  // strftime est utilisée pour formater la structure 'tm' en une chaîne de caractères.
  // "%H:%M:%S" formate l'heure en Heure:Minute:Seconde (format 24h).
  strftime(timeString, sizeof(timeString), "%H:%M:%S", &timeinfo);
  return timeString;
}

void initLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("Une erreur s'est produite lors du montage de LittleFS");
    messageSurLcd("Erreur FS", "Redemarrage...", 5000, true);
    delay(5000);
    ESP.restart();
  }
  Serial.println("LittleFS monte avec succes.");
}

void initWebSocket() {
  ws.onEvent(onEvent);
  server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
}

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) {  // Timeout de 5 secondes
    Serial.println("Erreur de recuperation de l'heure NTP");
    return "Time Error";
  }
  static char timeStr[64];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStr);
}

void messageSurLcd(String line1, String line2, unsigned long duration_ms, bool clearFirst) {
  // Tronquer les messages pour l'écran
  String displayLine1 = line1.substring(0, 16);
  String displayLine2 = line2.substring(0, 16);

  if (xSemaphoreTake(xMutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (clearFirst) {
      lcd.clear();
    }
    lcd.setCursor(0, 0);
    lcd.print(displayLine1);
    lcd.setCursor(0, 1);
    lcd.print(displayLine2);

    ligne1Lcd = line1;
    ligne2Lcd = line2;
    lcdMessageEndTime = (duration_ms == 0) ? 0 : millis() + duration_ms;
    lcdClearAfter = clearFirst;
    xSemaphoreGive(xMutexLCD);
  } else {
    Serial.println("Warning: Impossible d'acquérir le mutex LCD pour afficher le message.");
  }
}


void onEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  JSONVar statut;
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      client->text("{\"status\":\"connected\", \"message\":\"ESP32 pret a recevoir des commandes.\"}");
      // Envoyer immédiatement le statut système
      statut["action"] = "systemStatus";

      // Statut ESP32
      statut["cpuStatus"] = "Actif";

      // Statut capteur
      if (finger.verifyPassword()) {
        statut["sensorStatus"] = "Connecte (" + String(finger.getTemplateCount()) + " empreintes)";
      } else {
        statut["sensorStatus"] = "Erreur";
      }

      // Statut heure
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 0)) {
        statut["timeStatus"] = "Synchronisee";
      } else {
        statut["timeStatus"] = "Erreur";
      }
      client->text(JSON.stringify(statut));
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      ws.cleanupClients();
      break;
    case WS_EVT_DATA:
      {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
          data[len] = 0;
          String message = (char*)data;
          Serial.printf("Message WebSocket recu: %s\n", message.c_str());

          JSONVar messageJsonRecu = JSON.parse(message);
          if (JSON.typeof(messageJsonRecu) == "undefined") {
            client->text("{\"status\":\"error\", \"message\":\"Format JSON invalide.\"}");
            return;
          }

          String commandeRecu = (const char*)messageJsonRecu["command"];

          // Créer une nouvelle commande
          if (indexCommande >= MAX_COMMANDES) {
            indexCommande = 0;  // Gestion circulaire
          }
          commandeWebSocket* commandeEmisePourLaFileDattente = &commandePool[indexCommande++];
          commandeEmisePourLaFileDattente->client = client;
          commandeEmisePourLaFileDattente->command = commandeRecu;
          commandeEmisePourLaFileDattente->messageJsonRecu = messageJsonRecu;

          // Envoyer dans la file d'attente
          if (xQueueSend(xfileDattenteDesCommandeWebsockets, &commandeEmisePourLaFileDattente, 0) != pdPASS) {
            Serial.println("Erreur: File de commandes pleine");
            client->text("{\"status\":\"error\", \"message\":\"Systeme occupe, reessayez\"}");
          }
        }
      }
      break;
    case WS_EVT_PONG:  //non implementte pour l'instant
    case WS_EVT_ERROR:
      Serial.printf("WebSocket Error (client #%u): %s\n", client->id(), (char*)data);
      break;
  }
}

void lectureInformationsUtilisateur() {
  Serial.println("Chargement des données d'employés depuis LittleFS...");
  messageSurLcd("Chargement...", "Employes FP", 0, true);

  // Vérifier si le fichier existe
  if (!LittleFS.exists(FICHIER_EMPLOYE)) {
    Serial.println("Fichier employees.json non trouvé. Création d'une liste vide.");
    return;  // Pas d'employés à charger pour l'instant
  }

  File file = LittleFS.open(FICHIER_EMPLOYE, "r");
  if (!file) {
    Serial.println("Échec de l'ouverture de employees.json en lecture.");
    return;
  }

  // Nouveau code - Vérification taille fichier
  if (file.size() == 0) {
    Serial.println("Fichier employes.json vide - initialisation");
    file.close();
    LittleFS.remove(FICHIER_EMPLOYE);  // Supprime le fichier corrompu
    return;
  }

  // Lire tout le contenu du fichier dans une String
  String contenuDuFichier = file.readString();
  file.close();


  // Parser le JSON
  JSONVar tableauDesEmployes = JSON.parse(contenuDuFichier);

  if (JSON.typeof(tableauDesEmployes) == "undefined") {
    Serial.println("Erreur de parsing du JSON des employés.");
    return;
  }

  // Acquisition du mutex de la liste utilisateur
  if (xSemaphoreTake(xMutexListeUtilisateurEnregistre, pdMS_TO_TICKS(10000)) != pdTRUE) {
    Serial.println("Erreur pourquoi: Impossible d'acquérir le mutex pour charger les IDs.");
    return;  // Redémarrage contrôlé
  } else {
    //on efface l'ancien contenu
    listeDesUtilisateur.clear();

    if (JSON.typeof(tableauDesEmployes) == "array") {
      for (int i = 0; i < tableauDesEmployes.length(); i++) {
        JSONVar objetJsonEmploye = tableauDesEmployes[i];
        if (objetJsonEmploye.hasOwnProperty("id") && objetJsonEmploye.hasOwnProperty("nom")) {
          InformationsEmploye donneeEmploye;

          donneeEmploye.id = (uint8_t)(int)objetJsonEmploye["id"];
          donneeEmploye.nom = String((const char*)objetJsonEmploye["nom"]);
          donneeEmploye.checkIn = String((const char*)objetJsonEmploye["checkIn"]);
          donneeEmploye.checkOut = String((const char*)objetJsonEmploye["checkOut"]);
          donneeEmploye.dateConnection = String((const char*)objetJsonEmploye["dateConnection"]);
          listeDesUtilisateur.push_back(donneeEmploye);
        }
      }
    }

    // Trier la liste après le chargement
    std::sort(listeDesUtilisateur.begin(), listeDesUtilisateur.end(), [](const InformationsEmploye& a, const InformationsEmploye& b) {
      return a.id < b.id;
    });

    Serial.printf("Chargement de %d employés depuis LittleFS.\n", listeDesUtilisateur.size());
    xSemaphoreGive(xMutexListeUtilisateurEnregistre);
  }
}

void sauvegardeInformationUtilisateur() {
  Serial.println("Sauvegarde des données d'employés vers LittleFS...");
  JSONVar tableauDesEmployes = JSON.parse("[]");  // Crée un objet JSON racine (qui sera un tableau)
  String jsonEnregistree = "";
  //messageSurLcd("Sauvegarde...", "Employes FP", 3000, true);

  // Remplacer la section problématique par :
  if (xSemaphoreTake(xMutexListeUtilisateurEnregistre, pdMS_TO_TICKS(8000)) != pdTRUE) {
    Serial.println("Erreur: Impossible d'acquérir le mutex pour synchroniser la liste.");
    return;  // Ne pas redémarrer, simplement abandonner
  }
  // Conversion JSON rapide
  for (const auto& donneeEmploye : listeDesUtilisateur) {
    JSONVar objetJsonEmploye;

    objetJsonEmploye["id"] = donneeEmploye.id;
    objetJsonEmploye["nom"] = donneeEmploye.nom;
    objetJsonEmploye["checkIn"] = donneeEmploye.checkIn;
    objetJsonEmploye["checkOut"] = donneeEmploye.checkOut;
    objetJsonEmploye["dateConnection"] = donneeEmploye.dateConnection;
    tableauDesEmployes[tableauDesEmployes.length()] = objetJsonEmploye;  // Ajoute l'objet employé au tableau racine
  }
  jsonEnregistree = JSON.stringify(tableauDesEmployes);  // Convertit l'objet JSON en chaîne
  xSemaphoreGive(xMutexListeUtilisateurEnregistre);

  File file = LittleFS.open(FICHIER_EMPLOYE, "w");  // Ouvre le fichier en mode écriture (crée ou écrase)
  if (!file) {
    Serial.println("Échec de l'ouverture de employees.json en écriture.");
    return;
  }

  file.print(jsonEnregistree);  // Écrit la chaîne JSON dans le fichier
  file.close();

  Serial.println("Données d'employés sauvegardées avec succès.");
}

// Fonction pour récupérer tous les IDs d'empreintes actuellement stockés sur le capteur
std::set<uint8_t> getIDsSurCapteur(bool* mutexError = nullptr) {
  std::set<uint8_t> idsSurCapteur;
  Serial.println("Interrogation du capteur pour les IDs presents...");
  messageSurLcd("Scan Capteur", "IDs presents...", 0, false);
  uint16_t capacity = 127;  // Valeur fixe pour FPM10A
  Serial.printf("Capacite du capteur: %d\n", capacity);

  // Remplir la liste initiale des IDs a modifier plus tard avec le chargement depuis le LittleFS (pd signifi portable data/define)
  if (xSemaphoreTake(xMutexCapteurEmpreinte, pdMS_TO_TICKS(5000)) == pdTRUE) {

    for (uint16_t id = 1; id <= capacity; id++) {
      // loadModel tente de charger une empreinte. Si FINGERPRINT_OK, elle existe.
      if (finger.loadModel(id) == FINGERPRINT_OK) {
        idsSurCapteur.insert(id);
        // Serial.printf("ID %u trouve sur le capteur.\n", id); // Décommenter pour débogage
      }
      vTaskDelay(1);  // Permettre à d'autres tâches FreeRTOS de s'exécuter
    }
    xSemaphoreGive(xMutexCapteurEmpreinte);
    if (mutexError) *mutexError = false;
  } else {
    Serial.println("Erreur: Impossible d'acquérir le mutex capteur pour l'interrogation des IDs.");
    if (mutexError) *mutexError = true;
  }

  Serial.printf("Fin de l'interrogation. %d IDs trouvés sur le capteur.\n", idsSurCapteur.size());
  messageSurLcd("Capteur Scan OK", String(idsSurCapteur.size()) + " IDs", 0, false);

  return idsSurCapteur;
}


void synchroniserListeEmployesAvecCapteur() {
  Serial.println("Synchronisation de la liste des employés avec le capteur...");
  messageSurLcd("Synchro...", "Capteur/Fichier", 0, true);

  // 1. Obtenir les IDs présents sur le capteur
  std::set<uint8_t> idsCapteur = getIDsSurCapteur(nullptr);

  // 2. Acquérir le mutex pour la liste en mémoire
  if (xSemaphoreTake(xMutexListeUtilisateurEnregistre, pdMS_TO_TICKS(8000)) != pdTRUE) {
    Serial.println("Erreur: Impossible d'acquérir le mutex pour synchroniser la liste.");
    esp_restart();
  }

  // 3. Parcours et ajustement de listeDesUtilisateur
  // Utilisez un itérateur pour supprimer les éléments directement du vecteur
  auto it = listeDesUtilisateur.begin();
  while (it != listeDesUtilisateur.end()) {
    if (idsCapteur.find(it->id) == idsCapteur.end()) {
      // L'ID est dans notre liste de fichier, mais PAS sur le capteur
      Serial.printf("Employé ID %u (%s) trouvé dans le fichier mais absent du capteur. Supprimé de la liste.\n", it->id, it->nom.c_str());
      it = listeDesUtilisateur.erase(it);  // Supprime l'élément et avance l'itérateur
    } else {
      // L'ID est présent sur le capteur et dans notre liste, c'est bon.
      // On peut aussi le retirer du set idsCapteur pour identifier les IDs "nouveaux" après
      idsCapteur.erase(it->id);
      ++it;  // Passe à l'élément suivant
    }
  }

  // 4. Ajouter les IDs du capteur qui ne sont PAS dans notre liste (ceux qui restent dans idsCapteur)
  for (uint8_t idAbsentDuFichier : idsCapteur) {
    // Un ID est sur le capteur mais n'est pas dans notre liste de fichier.
    // On l'ajoute avec un nom générique. L'utilisateur devra le renommer via l'interface.
    InformationsEmploye nouvelEmployeInconnu = { idAbsentDuFichier, "Inconnu - " + String(idAbsentDuFichier), "", "", "" };
    bool exists = false;  //pour eviter les doublons
    for (const auto& u : listeDesUtilisateur) {
      if (u.id == idAbsentDuFichier) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      listeDesUtilisateur.push_back(nouvelEmployeInconnu);
      Serial.printf("ID %u trouvé sur le capteur mais absent du fichier. Ajouté comme 'Inconnu %u'.\n", idAbsentDuFichier, idAbsentDuFichier);
    }
  }

  // 5. Trier la liste mise à jour
  std::sort(listeDesUtilisateur.begin(), listeDesUtilisateur.end(), [](const InformationsEmploye& a, const InformationsEmploye& b) {
    return a.id < b.id;
  });

  xSemaphoreGive(xMutexListeUtilisateurEnregistre);  // Libérer le mutex

  // 6. Sauvegarder la liste mise à jour (fichier et capteur sont synchronisés)
  sauvegardeInformationUtilisateur();  // Sauvegarder la liste synchronisée
  Serial.println("Synchronisation terminée.");
  messageSurLcd("Synchro OK!", "Liste à jour", 2000, true);
}

// Fonction pour ajouter un employé à la liste en mémoire et sauvegarder
void ajouterEmployeAListe(uint8_t id, const String& nom) {
  if (xSemaphoreTake(xMutexListeUtilisateurEnregistre, portMAX_DELAY) == pdTRUE) {
    // Vérification si l'ID existe déjà
    bool idExists = false;
    for (const auto& emp : listeDesUtilisateur) {
      if (emp.id == id) {
        idExists = true;
        break;
      }
    }

    if (!idExists) {
      InformationsEmploye nouvelEmploye = { id, nom, "", "", "" };
      listeDesUtilisateur.push_back(nouvelEmploye);
      std::sort(listeDesUtilisateur.begin(), listeDesUtilisateur.end(), [](const InformationsEmploye& a, const InformationsEmploye& b) {
        return a.id < b.id;  //on classe les utilisateur ordre croissant
      });
      Serial.printf("Ajout de l'employé ID %u, Nom: %s à la liste en mémoire.\n", id, nom.c_str());
      xSemaphoreGive(xMutexListeUtilisateurEnregistre);  // Libérer le mutex avant la sauvegarde
      sauvegardeInformationUtilisateur();                // Sauvegarder après modification
    } else {
      Serial.printf("Erreur: L'employé avec l'ID %u existe déjà dans la liste.\n", id);
      xSemaphoreGive(xMutexListeUtilisateurEnregistre);
    }
  } else {
    Serial.println("Impossible d'acquérir le mutex pour ajouter l'employé.");
  }
}

// Nouvelle fonction pour supprimer un employé de la liste en mémoire et sauvegarder
void supprimerEmployeDeListe(uint8_t id) {
  if (xSemaphoreTake(xMutexListeUtilisateurEnregistre, portMAX_DELAY) == pdTRUE) {  //on place l'employer a supprimer a la fin du tableau avec remove_if et on recupere son index it
    auto it = std::remove_if(listeDesUtilisateur.begin(), listeDesUtilisateur.end(), [id](const InformationsEmploye& e) {
      return e.id == id;
    });

    if (it != listeDesUtilisateur.end()) {
      listeDesUtilisateur.erase(it, listeDesUtilisateur.end());
      Serial.printf("Employé ID %u supprimé de la liste en mémoire.\n", id);
      xSemaphoreGive(xMutexListeUtilisateurEnregistre);  // Libérer le mutex avant la sauvegarde
      sauvegardeInformationUtilisateur();                // Sauvegarder après modification
    } else {
      Serial.printf("Erreur: Employé ID %u non trouvé dans la liste.\n", id);
      xSemaphoreGive(xMutexListeUtilisateurEnregistre);
    }
  } else {
    Serial.println("Impossible d'acquérir le mutex pour supprimer l'employé.");
  }
}


// Fonction pour l'enregistrement d'une empreinte (MISE A JOUR)
int enregistrementEmpreinte(uint8_t id, const String& nom, String& messageErreur) {
  int codeRetour = -1;
  unsigned long chronoDepart = 0;
  messageErreur = "";

  // 1. Acquisition du mutex
  if (xSemaphoreTake(xMutexCapteurEmpreinte, pdMS_TO_TICKS(20000)) != pdTRUE) {
    messageErreur = "Mutex non acquis apres 20s";
    codeRetour = FINGERPRINT_MUTEX_TIMEOUT;
    goto nettoyage_final;
  }

  operationsCritiquesEnCours = true;
  tacheDeLectureEmpreinteEnabled = false;
  Serial.printf("acquisition du mutex et desactivation de la tache de lecture");
  tacheSyncNTPEnabled = false;
  tacheStatutSystemeEnabled = false;

  // 2. Vérification connexion capteur
  if (!finger.verifyPassword()) {
    messageErreur = "Capteur deconnecte";
    codeRetour = FINGERPRINT_NOT_CONNECTED;
    goto liberation_mutex;
  }

  // 3. Initialisation du processus
  messageSurLcd("Enregistrement:", "ID:" + String(id), 2000, true);
  Serial.printf("Début enregistrement ID %d\n", id);

  // 4. Capture première image
  chronoDepart = millis();
  while ((codeRetour = finger.getImage()) != FINGERPRINT_OK) {
    if (millis() - chronoDepart > 30000) {
      messageErreur = "Timeout capture premiere image";
      codeRetour = FINGERPRINT_TIMEOUT_1;
      goto liberation_mutex;
    }

    if (codeRetour != FINGERPRINT_NOFINGER) {
      messageErreur = "Erreur capture image 1 (0x" + String(codeRetour, HEX) + ")";
      goto liberation_mutex;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  Serial.println("Image 1 prise");

  // 5. Conversion image 1
  if ((codeRetour = finger.image2Tz(1)) != FINGERPRINT_OK) {
    messageErreur = "Erreur conversion image 1 (0x" + String(codeRetour, HEX) + ")";
    goto liberation_mutex;
  }

  // 6. Vérification doublon
  if ((codeRetour = finger.fingerFastSearch()) == FINGERPRINT_OK) {
    messageErreur = "Doublon trouve ID " + String(finger.fingerID);
    codeRetour = FINGERPRINT_DUPLICATE;
    goto liberation_mutex;
  } else if (codeRetour != FINGERPRINT_NOTFOUND) {
    messageErreur = "Erreur recherche doublon (0x" + String(codeRetour, HEX) + ")";
    goto liberation_mutex;
  }

  // 7. Attente retrait doigt
  messageSurLcd("Retirez", "le doigt", 0, true);
  chronoDepart = millis();
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    if (millis() - chronoDepart > 10000) {
      messageErreur = "Timeout retrait doigt";
      codeRetour = FINGERPRINT_TIMEOUT_2;
      goto liberation_mutex;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }

  // 8. Capture deuxième image
  messageSurLcd("Placez le", "même doigt", 0, true);
  chronoDepart = millis();
  while ((codeRetour = finger.getImage()) != FINGERPRINT_OK) {
    if (millis() - chronoDepart > 30000) {
      messageErreur = "Timeout capture deuxieme image";
      codeRetour = FINGERPRINT_TIMEOUT_3;
      goto liberation_mutex;
    }

    if (codeRetour != FINGERPRINT_NOFINGER) {
      messageErreur = "Erreur capture image 2 (0x" + String(codeRetour, HEX) + ")";
      goto liberation_mutex;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  Serial.println("Image 2 prise");

  // 9. Conversion image 2
  if ((codeRetour = finger.image2Tz(2)) != FINGERPRINT_OK) {
    messageErreur = "Erreur conversion image 2 (0x" + String(codeRetour, HEX) + ")";
    goto liberation_mutex;
  }

  // 10. Création modèle
  if ((codeRetour = finger.createModel()) != FINGERPRINT_OK) {
    messageErreur = "Erreur creation modele (0x" + String(codeRetour, HEX) + ")";
    goto liberation_mutex;
  }

  // 11. Stockage modèle
  if ((codeRetour = finger.storeModel(id)) != FINGERPRINT_OK) {
    messageErreur = "Erreur stockage modele (0x" + String(codeRetour, HEX) + ")";
    goto liberation_mutex;
  }


  messageErreur = "Enregistrement reussi!";
  messageSurLcd("Succès!", "ID:" + String(id), 3000, true);
  codeRetour = FINGERPRINT_OK;

  // Attente retrait doigt
  messageSurLcd("Retirez", "le doigt", 0, true);
  chronoDepart = millis();
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    if (millis() - chronoDepart > 10000) {
      messageErreur = "Timeout retrait doigt";
      codeRetour = FINGERPRINT_TIMEOUT_2;
      goto liberation_mutex;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }


liberation_mutex:
  xSemaphoreGive(xMutexCapteurEmpreinte);

nettoyage_final:
  //if (xSemaphoreTake(xMutexCapteurEmpreinte, pdMS_TO_TICKS(100)) == pdTRUE) xSemaphoreGive(xMutexCapteurEmpreinte);
  operationsCritiquesEnCours = false;
  tacheDeLectureEmpreinteEnabled = true;
  Serial.printf("liberation du mutex et activation de la tache de lecture");
  tacheSyncNTPEnabled = true;
  tacheStatutSystemeEnabled = true;

  // Si erreur non gérée mais codeRetour défini
  if (codeRetour != FINGERPRINT_OK && messageErreur == "") {
    messageErreur = "Erreur inconnue (Code: " + String(codeRetour) + ")";
  }

  if (codeRetour == FINGERPRINT_OK) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    // 12. Succès final
    ajouterEmployeAListe(id, nom);
    // Actualiser la liste côté client
    envoiDeLaListeUtilisateurEnregistre(nullptr);
  }

  return codeRetour;
}

// Fonction pour la suppression d'une empreinte(MISE A JOUR)
int suppressionEmpreinte(uint8_t id, String& messageErreur) {
  int codeRetour = -1;
  messageErreur = "";

  if (xSemaphoreTake(xMutexCapteurEmpreinte, pdMS_TO_TICKS(10000)) != pdTRUE) {
    messageErreur = "Systeme occupe";
    codeRetour = FINGERPRINT_MUTEX_TIMEOUT;
    goto nettoyage_final;
  }

  operationsCritiquesEnCours = true;
  tacheDeLectureEmpreinteEnabled = false;
  Serial.printf("acquisition du mutex et desactivation de la tache de lecture");
  tacheSyncNTPEnabled = false;
  tacheStatutSystemeEnabled = false;

  if (!finger.verifyPassword()) {
    messageErreur = "Capteur deconnecte";
    xSemaphoreGive(xMutexCapteurEmpreinte);
    codeRetour = FINGERPRINT_NOT_CONNECTED;
    goto liberation_mutex;
  }

  messageSurLcd("Suppression", "ID: " + String(id), 0, true);
  codeRetour = finger.deleteModel(id);

  switch (codeRetour) {
    case FINGERPRINT_OK:
      messageErreur = "Suppression reussie";
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      messageErreur = "Erreur communication capteur";
      break;
    case FINGERPRINT_BADLOCATION:
      messageErreur = "ID invalide ou empreinte inexistante";
      break;
    case FINGERPRINT_FLASHERR:
      messageErreur = "Erreur ecriture memoire flash";
      break;
    default:
      messageErreur = "Erreur inconnue (0x" + String(codeRetour, HEX) + ")";
      break;
  }

liberation_mutex:
  xSemaphoreGive(xMutexCapteurEmpreinte);

nettoyage_final:
  //if (xSemaphoreTake(xMutexCapteurEmpreinte, pdMS_TO_TICKS(100)) == pdTRUE) xSemaphoreGive(xMutexCapteurEmpreinte);
  operationsCritiquesEnCours = false;
  tacheDeLectureEmpreinteEnabled = true;
  Serial.printf("liberation du mutex et activation de la tache de lecture");
  tacheSyncNTPEnabled = true;
  tacheStatutSystemeEnabled = true;

  if (codeRetour == FINGERPRINT_OK) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
  }
  if (codeRetour == FINGERPRINT_OK) {
    supprimerEmployeDeListe(id);
    // Actualiser la liste côté client
    envoiDeLaListeUtilisateurEnregistre(nullptr);
  }

  return codeRetour;
}


// Fonction pour la vérification/identification d'une empreinte (MISE A JOUR)
void verifierEmpreinteDetaillee(ResultatVerification& resultat) {
  resultat.code = FINGERPRINT_UNKNOWN_ERROR;
  resultat.id = -1;
  resultat.message = "";

  if (xSemaphoreTake(xMutexCapteurEmpreinte, pdMS_TO_TICKS(2000)) != pdTRUE) {
    resultat.code = FINGERPRINT_MUTEX_TIMEOUT;
    resultat.message = "Systeme occupe";
    return;
  }
  if (verificationBloquee) {
    resultat.code = FINGERPRINT_BLOQUE;
    resultat.message = "Verification bloquee temporairement";
    return;
  }

  if (!finger.verifyPassword()) {
    xSemaphoreGive(xMutexCapteurEmpreinte);
    resultat.code = FINGERPRINT_NOT_CONNECTED;
    resultat.message = "Capteur deconnecte";
    // Envoyer l'erreur du capteur
    JSONVar erreurCapteur;
    erreurCapteur["action"] = "sensorError";
    erreurCapteur["message"] = "Capteur deconnecte";
    String jsonString = JSON.stringify(erreurCapteur);
    notifyClients(jsonString);
    return;
  }

  uint8_t p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    xSemaphoreGive(xMutexCapteurEmpreinte);
    resultat.code = p;
    resultat.message = "Erreur conversion image";
    Serial.printf("Erreur conversion image");
    return;
  }

  p = finger.fingerFastSearch();
  xSemaphoreGive(xMutexCapteurEmpreinte);

  if (p == FINGERPRINT_OK) {
    resultat.code = FINGERPRINT_OK;
    resultat.id = finger.fingerID;
    resultat.message = "Empreinte reconnue";
    essaisRestants = MAX_ESSAIS;
  } else if (p == FINGERPRINT_NOTFOUND) {
    resultat.code = FINGERPRINT_NOTFOUND;
    resultat.message = "Empreinte non reconnue";
    Serial.printf("Erreur Empreinte non reconnue");
  } else {
    resultat.code = p;
    resultat.message = "Erreur recherche empreinte";
  }
}

//Fonction pour notifier les clients connectee
void notifyClients(const String message) {
  ws.textAll(message);
  /*size_t numClients = ws.count();
  
  if (xSemaphoreTake(xMutexWebSocket, pdMS_TO_TICKS(20000)) != pdTRUE) {
    Serial.println("Erreur: Impossible d'acquérir le mutex pour charger les IDs.");
    vTaskDelay(pdMS_TO_TICKS(1000));  // petite pause
  }

  for (size_t i = 0; i < numClients; i++) {
    AsyncWebSocketClient* client = ws.client(i);
    if (client && client->status() == WS_CONNECTED && client->canSend()) {
      client->text(message);
    }
  }*/
}

// Fonction pour envoyer la liste des empreintes
void envoiDeLaListeUtilisateurEnregistre(AsyncWebSocketClient* client) {
  JSONVar response;
  JSONVar fingerprints = JSON.parse("[]");

  if (xSemaphoreTake(xMutexListeUtilisateurEnregistre, pdMS_TO_TICKS(1000)) == pdTRUE) {
    for (const auto& utilisateur : listeDesUtilisateur) {
      JSONVar fpObj;
      fpObj["id"] = utilisateur.id;
      fpObj["name"] = utilisateur.nom;
      fpObj["checkIn"] = utilisateur.checkIn;
      fpObj["checkOut"] = utilisateur.checkOut;
      fingerprints[fingerprints.length()] = fpObj;
    }
    xSemaphoreGive(xMutexListeUtilisateurEnregistre);
  }

  response["status"] = "success";
  response["action"] = "updateFingerprints";
  response["fingerprints"] = fingerprints;
  if (client == nullptr) {
    notifyClients(JSON.stringify(response));  // Envoyer à tous les clients
  } else {
    client->text(JSON.stringify(response));  // Envoyer au client spécifique
  }
  Serial.println("Liste d'empreintes envoyee");
}


// Tâche de traitement des empreintes (MISE A JOUR)
void tacheDeLectureEmpreinte(void* pvParameters) {
  vTaskDelay(pdMS_TO_TICKS(10000));
  (void)pvParameters;
  static ContexteVerification contexte = {
    .etatActuel = ETAT_ATTENTE_DOIGT,
    .essaisRestants = MAX_ESSAIS,
    .chronoDebut = 0,
    .idUtilisateur = -1,
    .dernierResultat = { .code = FINGERPRINT_NOFINGER, .id = -1, .message = "" }
  };

  static unsigned long lastDateCheck = 0;

  for (;;) {
    // Contrôle global d'activation de la tâche
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!tacheDeLectureEmpreinteEnabled) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Vérifier le changement de date périodiquement
    if (millis() - lastDateCheck > DATE_CHECK_INTERVAL) {
      String newDate = getFormattedTime().substring(0, 10);
      lastDateCheck = millis();

      if (newDate != currentDate) {
        // Jour a changé - réinitialiser tous les pointages
        currentDate = newDate;

        if (xSemaphoreTake(xMutexListeUtilisateurEnregistre, pdMS_TO_TICKS(1000)) == pdTRUE) {
          for (auto& employe : listeDesUtilisateur) {
            employe.dateConnection = currentDate;
            employe.checkIn = "";
            employe.checkOut = "";
          }
          xSemaphoreGive(xMutexListeUtilisateurEnregistre);

          // Sauvegarder les modifications
          sauvegardeInformationUtilisateur();

          // Notifier les clients
          JSONVar resetNotification;
          resetNotification["action"] = "dayReset";
          resetNotification["date"] = currentDate;
          notifyClients(JSON.stringify(resetNotification));

          Serial.println("Réinitialisation quotidienne effectuée");
        }
      }
    }


    switch (contexte.etatActuel) {
      //--------------------------------------------------
      case ETAT_ATTENTE_DOIGT:
        operationsCritiquesEnCours = false;

        // Vérifier si la tâche est temporairement désactivée
        if (!tacheDeLectureEmpreinteEnabled) {
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          break;
        }

        // Détection présence doigt
        if (xSemaphoreTake(xMutexCapteurEmpreinte, pdMS_TO_TICKS(3000)) == pdTRUE) {
          uint16_t detection = finger.getImage();

          if (detection == FINGERPRINT_OK) {
            // Doigt détecté, passer à l'étape suivante
            contexte.etatActuel = ETAT_VERIFICATION_EN_COURS;
            contexte.chronoDebut = millis();
            operationsCritiquesEnCours = true;
            xSemaphoreGive(xMutexCapteurEmpreinte);
          } else if (detection == FINGERPRINT_NOFINGER) {
            // Aucun doigt, rester en attente
            contexte.etatActuel = ETAT_ATTENTE_DOIGT;
            xSemaphoreGive(xMutexCapteurEmpreinte);
            vTaskDelay(pdMS_TO_TICKS(50));
          } else {
            // Autre erreur, afficher et attendre un peu
            Serial.print("Erreur getImage: 0x");
            Serial.println(detection, HEX);
            contexte.etatActuel = ETAT_ATTENTE_DOIGT;
            xSemaphoreGive(xMutexCapteurEmpreinte);
            vTaskDelay(pdMS_TO_TICKS(1500));
          }
        } else {
          Serial.printf("capteur occupe");
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        break;
      //--------------------------------------------------
      case ETAT_VERIFICATION_EN_COURS:
        // Vérifier si la tâche a été désactivée pendant l'opération
        if (!tacheDeLectureEmpreinteEnabled) {
          operationsCritiquesEnCours = false;
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          break;
        }
        verifierEmpreinteDetaillee(contexte.dernierResultat);

        // Gestion spéciale si la vérification a été interrompue
        if (contexte.dernierResultat.code == FINGERPRINT_NOT_CONNECTED || contexte.dernierResultat.code == FINGERPRINT_MUTEX_TIMEOUT) {
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          operationsCritiquesEnCours = false;
          break;
        }

        if (contexte.dernierResultat.code == FINGERPRINT_OK) {
          contexte.idUtilisateur = contexte.dernierResultat.id;
          contexte.etatActuel = ETAT_RECONNAISSANCE_REUSSIE;
        } else if (contexte.dernierResultat.code == FINGERPRINT_NOFINGER) {
          // Aucun doigt détecté après la première vérification
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          operationsCritiquesEnCours = false;
        } else if (contexte.dernierResultat.code == FINGERPRINT_NOTFOUND) {
          contexte.etatActuel = ETAT_ERREUR_RECONNAISSANCE;
        } else {
          // Toute autre erreur -> retour à l'attente sans pénalité
          String ligne1 = "Erreur capteur";
          String ligne2 = "Code: " + String(contexte.dernierResultat.code);
          messageSurLcd(ligne1, ligne2, 2000, true);
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          operationsCritiquesEnCours = false;
        }
        break;

      //--------------------------------------------------
      case ETAT_RECONNAISSANCE_REUSSIE:
        contexte.essaisRestants = MAX_ESSAIS;


        // ... (traitement du pointage) ...
        if (!tacheDeLectureEmpreinteEnabled) {
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          operationsCritiquesEnCours = false;
          break;
        }

        //train=tement
        if (contexte.idUtilisateur > 0) {
          // Variables pour stocker l'état complet
          bool updatePerformed = false;
          String employeeName = "";
          String newCheckIn = "";
          String newCheckOut = "";
          String newDate = "";
          bool isCheckInAction = false;

          // 1. Obtenir le temps de manière sécurisée
          String fullTime = getFormattedTime();
          String currentDate = fullTime.substring(0, 10);
          String currentTime = fullTime.substring(11);

          // 2. Traitement sous mutex
          if (xSemaphoreTake(xMutexListeUtilisateurEnregistre, pdMS_TO_TICKS(1000)) == pdTRUE) {
            for (auto& employe : listeDesUtilisateur) {
              if (employe.id == contexte.idUtilisateur) {
                employeeName = employe.nom;

                // Nouveau jour = réinitialisation
                if (employe.dateConnection != currentDate) {
                  employe.dateConnection = currentDate;
                  employe.checkIn = "";
                  employe.checkOut = "";
                  updatePerformed = true;  // La réinitialisation est une mise à jour
                }

                // Gestion pointage - Logique corrigée
                if (employe.checkIn == "") {
                  employe.checkIn = currentTime;
                  newCheckIn = currentTime;
                  newCheckOut = employe.checkOut;  // Conserver l'ancienne valeur
                  isCheckInAction = true;
                  updatePerformed = true;
                } else if (employe.checkOut == "") {
                  employe.checkOut = currentTime;
                  newCheckIn = employe.checkIn;  // Conserver l'ancienne valeur
                  newCheckOut = currentTime;
                  isCheckInAction = false;
                  updatePerformed = true;
                }

                newDate = employe.dateConnection;
                break;
              }
            }
            xSemaphoreGive(xMutexListeUtilisateurEnregistre);
          }

          // 3. Affichage et communication
          operationsCritiquesEnCours = true;
          if (updatePerformed) {
            // Afficher le bon message
            if (isCheckInAction) {
              messageSurLcd("Check-in", employeeName, 3000, true);
            } else {
              messageSurLcd("Check-out", employeeName, 3000, true);
            }

            // Sauvegarder les modifications
            sauvegardeInformationUtilisateur();

            // Préparer les données pour l'envoi
            JSONVar employeeData;
            employeeData["action"] = "updatePresence";
            employeeData["id"] = contexte.idUtilisateur;
            employeeData["name"] = employeeName;
            employeeData["checkIn"] = newCheckIn;
            employeeData["checkOut"] = newCheckOut;
            employeeData["date"] = newDate;

            // Envoyer via WebSocket
            if (JSON.typeof(employeeData) != "undefined") {
              notifyClients(JSON.stringify(employeeData));
            }
          } else {
            messageSurLcd("Erreur", "Deja pointe", 3000, true);
          }
        }
        contexte.etatActuel = ETAT_ATTENTE_DOIGT;
        operationsCritiquesEnCours = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
        break;

      //--------------------------------------------------
      case ETAT_ERREUR_RECONNAISSANCE:
        // Vérifier interruption avant traitement
        if (!tacheDeLectureEmpreinteEnabled) {
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          operationsCritiquesEnCours = false;
          break;
        }

        contexte.essaisRestants--;

        if (contexte.essaisRestants > 0) {
          // ... (traitement erreur normale) ...
          // Préparer le message
          String ligne1 = "Echec verification";
          String ligne2 = "Essais rest: " + String(contexte.essaisRestants);
          messageSurLcd(ligne1, ligne2, 1500, true);
          Serial.printf("Echec verification\n");
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
          contexte.etatActuel = ETAT_SYSTEME_BLOQUE;
          contexte.chronoDebut = millis();

          // Notification blocage
          JSONVar notification;
          notification["action"] = "verificationBloquee";
          notification["message"] = "Trop de tentatives echouees";
          notification["delai"] = DELAI_BLOQUAGE / 1000;
          notifyClients(JSON.stringify(notification));
          operationsCritiquesEnCours = true;
          vTaskDelay(pdMS_TO_TICKS(50));
        }
        break;

      //--------------------------------------------------
      case ETAT_SYSTEME_BLOQUE:
        // Vérifier si le blocage doit être interrompu
        if (!tacheDeLectureEmpreinteEnabled) {
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          operationsCritiquesEnCours = false;
          break;
        }

        // ... (traitement du blocage) ...
        // Clignotement LED
        digitalWrite(LED_BUILTIN, (millis() % 1000) < 500 ? HIGH : LOW);

        // Calcul temps restant
        unsigned long tempsEcoule = millis() - contexte.chronoDebut;
        if (tempsEcoule >= DELAI_BLOQUAGE) {
          contexte.essaisRestants = MAX_ESSAIS;
          digitalWrite(LED_BUILTIN, LOW);
          contexte.etatActuel = ETAT_ATTENTE_DOIGT;
          break;
        }

        operationsCritiquesEnCours = true;

        // Affichage décompte
        int secondesRestantes = (DELAI_BLOQUAGE - tempsEcoule) / 1000;
        String message = "Reessai dans: " + String(secondesRestantes) + "s";
        messageSurLcd("Acces bloque!", message, 0, true);
        Serial.printf("%s\n", message.c_str());
        operationsCritiquesEnCours = false;
        break;
    }
  }
}


// Tâche de traitement des commandes (MISE A JOUR)
void tacheDetraitementDesCommandesWebSockets(void* pvParameters) {
  commandeWebSocket* commandeRecuDeLaFileDattente;

  for (;;) {
    if (xQueueReceive(xfileDattenteDesCommandeWebsockets, &commandeRecuDeLaFileDattente, portMAX_DELAY) == pdPASS) {
      if (!(commandeRecuDeLaFileDattente->client->status() == WS_CONNECTED)) continue;  //on ignore la commande si le client n'est pas connecté

      Serial.printf("Traitement commande: %s\n", commandeRecuDeLaFileDattente->command.c_str());

      if (commandeRecuDeLaFileDattente->command == "enrollFingerprint") {

        if (commandeRecuDeLaFileDattente->messageJsonRecu.hasOwnProperty("id")) {
          uint8_t id = (int)commandeRecuDeLaFileDattente->messageJsonRecu["id"];
          String nom = (const char*)commandeRecuDeLaFileDattente->messageJsonRecu["name"];

          String messageErreur;
          int resultat = enregistrementEmpreinte(id, nom, messageErreur);

          JSONVar response;
          response["action"] = (resultat == FINGERPRINT_OK) ? "enrollComplete" : "enrollFailed";
          response["id"] = id;
          response["errorCode"] = resultat;
          response["message"] = messageErreur;

          commandeRecuDeLaFileDattente->client->text(JSON.stringify(response));
        } else {
          commandeRecuDeLaFileDattente->client->text("{\"status\":\"error\", \"message\":\"Commande 'enrollFingerprint' requiert un 'id'.\"}");
        }
      } else if (commandeRecuDeLaFileDattente->command == "deleteFingerprint") {
        if (commandeRecuDeLaFileDattente->messageJsonRecu.hasOwnProperty("id")) {
          uint8_t id = (int)commandeRecuDeLaFileDattente->messageJsonRecu["id"];

          String messageErreur;
          int resultat = suppressionEmpreinte(id, messageErreur);

          JSONVar response;
          response["action"] = (resultat == FINGERPRINT_OK) ? "deleteComplete" : "deleteFailed";
          response["id"] = id;
          response["errorCode"] = resultat;
          response["message"] = messageErreur;

          commandeRecuDeLaFileDattente->client->text(JSON.stringify(response));
        } else {
          commandeRecuDeLaFileDattente->client->text("{\"status\":\"error\", \"message\":\"Commande 'deleteFingerprint' requiert un 'id'.\"}");
        }
      } else if (commandeRecuDeLaFileDattente->command == "getFingerprints") {
        envoiDeLaListeUtilisateurEnregistre(commandeRecuDeLaFileDattente->client);
      } else {
        commandeRecuDeLaFileDattente->client->text("{\"status\":\"error\", \"message\":\"Commande inconnue\"}");
      }
    }
  }
}

//tache de synchronisation periodique
void tacheSyncNTP(void* pvParameters) {
  const int MAX_TENTATIVES = 3;
  const char* serveurs[] = { "pool.ntp.org", "time.google.com", "time.windows.com", "time.nist.gov", "ntp.ubuntu.com" };
  const int NB_SERVEURS = sizeof(serveurs) / sizeof(serveurs[0]);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));
    bool succes = false;
    int tentatives = 0;
    if (!tacheSyncNTPEnabled) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // Essayer plusieurs serveurs
    operationsCritiquesEnCours = true;
    while (tentatives < MAX_TENTATIVES && !succes) {

      const char* serveur = serveurs[tentatives % NB_SERVEURS];
      configTime(gmtOffset_sec, daylightOffset_sec, serveur);

      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 5000)) {  // Timeout de 10s
        succes = true;
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S %d/%m/%Y", &timeinfo);
        Serial.printf("NTP synchronise avec %s: %s\n", serveur, timeStr);

        // Afficher confirmation sur LCD
        messageSurLcd("NTP Sync OK", serveur, 3000, true);
      } else {
        Serial.printf("Echec NTP (%s), tentative %d/%d\n", serveur, tentatives + 1, MAX_TENTATIVES);

        // Affichage intermédiaire sur LCD
        String ligne1 = "Erreur NTP " + String(tentatives + 1);
        String ligne2 = "Serveur: " + String(serveur);
        messageSurLcd(ligne1, ligne2, 3000, true);

        tentatives++;
        vTaskDelay(pdMS_TO_TICKS(1000));  // Attente courte entre tentatives
      }
    }

    // Gestion des échecs critiques
    if (!succes) {
      Serial.println("Echec critique synchronisation NTP!");

      // 1. Affichage LCD prioritaire
      messageSurLcd("ERREUR NTP", "Sync impossible!", 5000, true);

      // 2. Notification aux clients WebSocket
      JSONVar erreurNTP;
      erreurNTP["action"] = "ntpError";
      erreurNTP["message"] = "Echec synchronisation heure apres " + String(MAX_TENTATIVES) + " tentatives";
      notifyClients(JSON.stringify(erreurNTP));
    }

    operationsCritiquesEnCours = false;
    // Attendre 30min avant prochaine synchro
    vTaskDelay(pdMS_TO_TICKS(1800000));
  }
}

//tache pour l'envoi du status  du systeme
void tacheStatutSysteme(void* pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    if (!tacheStatutSystemeEnabled) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    JSONVar statut;
    statut["action"] = "systemStatus";

    // Statut ESP32
    statut["cpuStatus"] = "Actif";

    // Nouveau : compter le nombre d'empreintes avec gestion d'erreur mutex
    bool mutexError = false;
    std::set<uint8_t> nombreEmpreinte = getIDsSurCapteur(&mutexError);
    if (mutexError) {
      statut["sensorStatus"] = "Erreur accès capteur (mutex)";
      statut["nbEmpreintes"] = -1;
    } else {
      statut["sensorStatus"] = "Connecte (" + String(nombreEmpreinte.size()) + " empreintes)";
      statut["nbEmpreintes"] = (int)nombreEmpreinte.size();
    }

    // Statut heure
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      statut["timeStatus"] = "Synchronisee";
    } else {
      statut["timeStatus"] = "Erreur";
    }

    String jsonString = JSON.stringify(statut);
    notifyClients(jsonString);

    vTaskDelay(pdMS_TO_TICKS(30000));  // Toutes les 30 secondes
  }
}


/**
 * @brief Efface tout le contenu de l'écran LCD et repositionne le curseur.
 */
void efface() {
  lcd.clear();          // Commande pour effacer complètement l'écran.
  lcd.setCursor(0, 1);  // Déplace le curseur à la première colonne de la deuxième ligne par défaut.
}

/**
 * @brief Efface le contenu d'une ligne spécifique sur l'écran LCD.
 * Utile pour rafraîchir une partie de l'affichage sans tout effacer.
 * @param positionColonne La colonne de départ pour effacer (généralement 0).
 * @param positionLigne La ligne à effacer (0 pour la première, 1 pour la deuxième).
 */
void effaceLigne(uint8_t positionColonne, uint8_t positionLigne) {
  for (int i = 0; i < 16; ++i) {      // Boucle à travers les 16 colonnes de la ligne.
    lcd.setCursor(i, positionLigne);  // Positionne le curseur à chaque colonne.
    lcd.print(" ");                   // Écrit un espace pour "effacer" le caractère précédent.
  }
  lcd.setCursor(positionColonne, positionLigne);  // Repositionne le curseur à la position de départ.
}