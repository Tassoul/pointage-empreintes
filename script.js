
// Constantes globales
const WS_RECONNECT_DELAY = 5000;
const REFRESH_INTERVAL = 60000;
const MAX_RECONNECT_ATTEMPTS = 10;

// Variables globales
let websocket;
let reconnectAttempts = 0;
let refreshInterval;
let isConnected = false;
let isProcessing = false;
let fingerprintsData = []; // Stocker les données des empreintes

// Éléments de l'interface
const elements = {
    enrollBtn: document.getElementById('enrollBtn'),
    deleteBtn: document.getElementById('deleteBtn'),
    refreshBtn: document.getElementById('refreshBtn'),
    enrollId: document.getElementById('enrollId'),
    enrollName: document.getElementById('enrollName'),
    deleteId: document.getElementById('deleteId'),
    fingerprintsTable: document.getElementById('fingerprintsTable'),
    systemMessages: document.getElementById('systemMessages'),
    statutESP32: document.getElementById('statutESP32'),
    statutCapteur: document.getElementById('statutCapteur'),
    statutHeure: document.getElementById('statutHeure'),
    lastUpdateTime: document.getElementById('lastUpdateTime'),
    alertModal: document.getElementById('alertModal'),
    modalTitle: document.getElementById('modalTitle'),
    modalMessage: document.getElementById('modalMessage'),
    closeBtn: document.querySelector('.close-btn')
};

// Messages d'erreur
const errorMessages = {
    FINGERPRINT_MUTEX_TIMEOUT: "Système occupé, veuillez réessayer",
    FINGERPRINT_NOT_CONNECTED: "Capteur déconnecté",
    FINGERPRINT_TIMEOUT_1: "Timeout capture première image",
    FINGERPRINT_TIMEOUT_2: "Timeout retrait du doigt",
    FINGERPRINT_TIMEOUT_3: "Timeout capture deuxième image",
    FINGERPRINT_DUPLICATE: "Empreinte déjà enregistrée",
    FINGERPRINT_BLOQUE: "Système temporairement bloqué",
    FINGERPRINT_UNKNOWN_ERROR: "Erreur inconnue",
    WS_CONNECTION_FAILED: "Connexion au serveur échouée",
    WS_SERVER_DOWN: "Serveur indisponible"
};

// Initialisation de l'application
function initApplication() {
    // Configuration des écouteurs d'événements
    elements.enrollBtn.addEventListener('click', handleEnroll);
    elements.deleteBtn.addEventListener('click', handleDelete);
    elements.refreshBtn.addEventListener('click', refreshFingerprints);
    elements.closeBtn.addEventListener('click', closeModal);
    elements.enrollId.addEventListener('input', () => {elements.enrollId.classList.remove('error-field');});
    elements.deleteId.addEventListener('input', () => {elements.deleteId.classList.remove('error-field');});
    
    // Désactiver l'interface initialement
    disableInterface();
    
    // Initialiser la connexion WebSocket
    initWebSocket();
}

// Gestion de la connexion WebSocket
function initWebSocket() {
    const wsUrl = `ws://${window.location.hostname}/ws`;
    console.log("Tentative de connexion WebSocket:", wsUrl);
    
    addSystemMessage("Tentative de connexion au système ESP32...", "info");
    updateStatus("ESP32: Connexion...", null, null);
    
    websocket = new WebSocket(wsUrl);
    
    websocket.onopen = function(event) {
        handleWebSocketOpen(event);
    };
    
    websocket.onclose = function(event) {
        handleWebSocketClose(event);
    };
    
    websocket.onmessage = function(event) {
        handleWebSocketMessage(event);
    };
    
    websocket.onerror = function(event) {
        handleWebSocketError(event);
    };
}

function handleWebSocketOpen(event) {
    reconnectAttempts = 0;
    isConnected = true;
    
    addSystemMessage("Connexion au système ESP32 établie", "success");
    updateStatus("ESP32: Connecté", null, null);
    
    // Activer l'interface
    enableInterface();
    
    // Demander la liste initiale des empreintes
    refreshFingerprints();
    
    // Configurer l'actualisation périodique
    refreshInterval = setInterval(refreshFingerprints, REFRESH_INTERVAL);
}

function handleWebSocketClose(event) {
    isConnected = false;
    clearInterval(refreshInterval);
    if (event.code === 1006) {
        addSystemMessage("Connexion perdue. Reconnexion...", "warning");
    }
    
    addSystemMessage(`Connexion fermée (code: ${event.code})`, "warning");
    updateStatus("ESP32: Déconnecté", null, null);
    
    // Désactiver l'interface
    disableInterface();
    
    // Tenter de se reconnecter
    if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
        reconnectAttempts++;
        const delay = Math.min(WS_RECONNECT_DELAY * Math.pow(2, reconnectAttempts), 30000);
        
        addSystemMessage(`Tentative de reconnexion #${reconnectAttempts} dans ${delay/1000}s...`, "info");
        setTimeout(initWebSocket, delay);
    } else {
        addSystemMessage("Échec de reconnexion. Veuillez rafraîchir la page.", "error");
    }
}

function handleWebSocketError(event) {
    addSystemMessage("Erreur de connexion WebSocket", "error");
    updateStatus("ESP32: Erreur", null, null);
}

function handleWebSocketMessage(event) {
    try {
        const message = JSON.parse(event.data);
        console.log("Message reçu:", message);
        
        // Journalisation des messages système
        if (message.message) {
            addSystemMessage(message.message, message.status || "info");
        }
        
        // Traitement des actions spécifiques
        switch (message.action) {
            case "updatePresence":
                updateEmployeeInTable(message);
                highlightEmployeeRow(message.id);
                break;
                
            case "updateFingerprints":
                fingerprintsData = message.fingerprints || [];
                updateFingerprintsTable(fingerprintsData);
                break;
                
            case "enrollComplete":
                isProcessing = false;
                showAlert("Enregistrement Réussi", `ID ${message.id} enregistré avec succès`, "success");
                resetButton(elements.enrollBtn);
                resetEnrollForm();
                setTimeout(refreshFingerprints, 300);
                break;
                
            case "enrollFailed":
                isProcessing = false;
                const enrollError = errorMessages[message.errorCode] || message.message;
                showAlert("Échec Enregistrement", `ID ${message.id}: ${enrollError}`, "error");
                resetButton(elements.enrollBtn);
                break;
                
            case "deleteComplete":
                isProcessing = false;
                showAlert("Suppression Réussie", `ID ${message.id} supprimé avec succès`, "success");
                resetButton(elements.deleteBtn);
                resetDeleteForm();
                // Supprimer visuellement la ligne sans recharger toute la table
                removeEmployeeFromTable(message.id);
                break;
                
            case "deleteFailed":
                isProcessing = false;
                const deleteError = errorMessages[message.errorCode] || message.message;
                showAlert("Échec Suppression", `ID ${message.id}: ${deleteError}`, "error");
                resetButton(elements.deleteBtn);
                break;
                
            case "verificationBloquee":
                showAlert("Système Bloqué", `Trop de tentatives. Réessai dans ${message.delai}s`, "warning");
                break;
                
            case "ntpError":
                updateStatus(null, "Heure: Erreur sync", null);
                showAlert("Erreur Synchronisation", message.message, "warning");
                break;
                
            case "sensorError":
                updateStatus("Capteur: Erreur", null, null);
                showAlert("Erreur Capteur", message.message, "error");
                break;
                
            case "systemStatus":
                updateStatus(
                    message.sensorStatus ? `Capteur: ${message.sensorStatus}` : null,
                    message.timeStatus ? `Heure: ${message.timeStatus}` : null,
                    message.cpuStatus ? `ESP32: ${message.cpuStatus}` : null
                );
                break;

            case "dayReset":
                handleDayReset(message.date);
                break;
        }
    } catch (e) {
        console.error("Erreur de traitement du message:", e, event.data);
        addSystemMessage("Erreur de traitement du message système", "error");
    }
}

// Fonctions d'interface
function enableInterface() {
    elements.enrollBtn.disabled = false;
    elements.deleteBtn.disabled = false;
    elements.refreshBtn.disabled = false;
    elements.enrollId.disabled = false;
    elements.enrollName.disabled = false;
    elements.deleteId.disabled = false;
}

function disableInterface() {
    elements.enrollBtn.disabled = true;
    elements.deleteBtn.disabled = true;
    elements.refreshBtn.disabled = true;
    elements.enrollId.disabled = true;
    elements.enrollName.disabled = true;
    elements.deleteId.disabled = true;
}

function resetEnrollForm() {
    elements.enrollId.value = "";
    elements.enrollName.value = "";
    elements.enrollId.classList.remove('error-field');
    elements.enrollName.classList.remove('error-field');
    elements.enrollId.disabled = false;
    elements.enrollName.disabled = false;
}

function resetDeleteForm() {
    elements.deleteId.value = "";
    elements.deleteId.classList.remove('error-field');
    elements.deleteId.disabled = false;
}

function validateEnrollFields() {
    if (isProcessing) {
        showAlert("Opération en cours", "Veuillez patienter...", "warning");
        return false;
    }
    
    const id = parseInt(elements.enrollId.value);
    const name = elements.enrollName.value.trim();
    
    // Validation ID numérique
    if (isNaN(id) || id < 1 || id > 127) {
        showAlert("ID Invalide", "L'ID doit être entre 1 et 127", "error");
        elements.enrollId.classList.add('error-field');
        elements.enrollId.focus();
        return false;
    }
    
    // Validation ID existant (version optimisée)
    const existingIds = fingerprintsData.map(fp => fp.id);
    if (existingIds.includes(id)) {
        showAlert("ID Existant", "Cet ID est déjà utilisé", "warning");
        elements.enrollId.classList.add('error-field');
        elements.enrollId.focus();
        return false;
    }
    
    // Validation nom
    if (name === "") {
        showAlert("Nom Manquant", "Veuillez entrer un nom d'employé", "error");
        elements.enrollName.classList.add('error-field');
        elements.enrollName.focus();
        return false;
    }
    
    return true;
}

function validateDeleteField() {
    if (isProcessing) {
        showAlert("Opération en cours", "Veuillez patienter...", "warning");
        return false;
    }
    
    const id = parseInt(elements.deleteId.value);
    
    // Validation ID numérique
    if (isNaN(id) || id < 1 || id > 127) {
        showAlert("ID Invalide", "L'ID doit être entre 1 et 127", "error");
        elements.deleteId.classList.add('error-field');
        elements.deleteId.focus();
        return false;
    }
    
    // Vérification existence ID (version optimisée)
    const existingIds = fingerprintsData.map(fp => fp.id);
    if (!existingIds.includes(id)) {
        showAlert("ID Inconnu", "Cet ID n'existe pas dans la base", "warning");
        elements.deleteId.classList.add('error-field');
        elements.deleteId.focus();
        return false;
    }
    
    return true;
}

// Fonctions de communication
function refreshFingerprints() {
    if (isConnected && !isProcessing) {
        sendWebSocketCommand({ command: "getFingerprints" });
    }
}

function handleEnroll() {
    if (!isConnected) {
        showAlert("Non Connecté", "Connexion au système non établie", "error");
        return;
    }
    
    if (!validateEnrollFields()) return;
    
    const id = parseInt(elements.enrollId.value);
    const name = elements.enrollName.value.trim();
    
    isProcessing = true;
    setButtonLoading(elements.enrollBtn);
    
    sendWebSocketCommand({
        command: "enrollFingerprint",
        id: id,
        name: name
    });
    
    showAlert("Enregistrement", `Enregistrement ID ${id} en cours...`, "info", 3000);
}

function handleDelete() {
    if (!isConnected) {
        showAlert("Non Connecté", "Connexion au système non établie", "error");
        return;
    }
    
    if (!validateDeleteField()) return;
    
    const id = parseInt(elements.deleteId.value);
    
    isProcessing = true;
    setButtonLoading(elements.deleteBtn);
    
    sendWebSocketCommand({
        command: "deleteFingerprint",
        id: id
    });
    
    showAlert("Suppression", `Suppression ID ${id} en cours...`, "info", 3000);
}

function sendWebSocketCommand(command) {
    if (isConnected && websocket.readyState === WebSocket.OPEN) {
        websocket.send(JSON.stringify(command));
    } else {
        addSystemMessage("Impossible d'envoyer la commande: connexion perdue", "error");
        isProcessing = false;
        resetButton(elements.enrollBtn);
        resetButton(elements.deleteBtn);
    }
}

// Fonctions de gestion de l'interface
function updateFingerprintsTable(fingerprints) {
    const tableBody = elements.fingerprintsTable.querySelector('tbody');
    tableBody.innerHTML = '';
    
    if (fingerprints && fingerprints.length > 0) {
        fingerprints.forEach(fp => {
            const row = tableBody.insertRow();
            row.classList.add('fade-in');
            
            // ID
            let idCell = row.insertCell(0);
            idCell.textContent = fp.id;
            idCell.setAttribute('data-label', 'ID');
            
            // Nom
            let nameCell = row.insertCell(1);
            nameCell.textContent = fp.name || "N/A";
            nameCell.setAttribute('data-label', 'Nom');
            
            // Arrivée
            let inCell = row.insertCell(2);
            inCell.textContent = fp.checkIn || "-";
            inCell.setAttribute('data-label', 'Arrivée');
            
            // Départ
            let outCell = row.insertCell(3);
            outCell.textContent = fp.checkOut || "-";
            outCell.setAttribute('data-label', 'Départ');
            
            // Statut
            let statusCell = row.insertCell(4);
            const status = getPresenceStatus(fp.checkIn, fp.checkOut);
            statusCell.textContent = status;
            statusCell.className = `status-${status}`;
            statusCell.setAttribute('data-label', 'Statut');
        });
    } else {
        const row = tableBody.insertRow();
        row.classList.add('fade-in');
        const cell = row.insertCell(0);
        cell.colSpan = 5;
        cell.textContent = "Aucune empreinte enregistrée";
        cell.className = "message-chargement";
        cell.setAttribute('data-label', '');
    }
    
    // Mise à jour de l'heure de dernière mise à jour
    const now = new Date();
    elements.lastUpdateTime.textContent = now.toLocaleTimeString();
    elements.lastUpdateTime.title = now.toLocaleString();
}

function updateEmployeeInTable(employee) {
    const rows = elements.fingerprintsTable.querySelectorAll('tbody tr');
    let found = false;
    
    for (const row of rows) {
        if (row.cells[0].textContent == employee.id) {
            // Mettre à jour les informations
            row.cells[1].textContent = employee.name || "N/A";
            row.cells[2].textContent = employee.checkIn || "-";
            row.cells[3].textContent = employee.checkOut || "-";
            
            // Mettre à jour le statut
            const status = getPresenceStatus(employee.checkIn, employee.checkOut);
            row.cells[4].textContent = status;
            row.cells[4].className = `status-${status}`;
            
            found = true;
            break;
        }
    }
    
    if (!found) {
        refreshFingerprints();
    }
    
    // Mise à jour de l'heure de dernière mise à jour
    const now = new Date();
    elements.lastUpdateTime.textContent = now.toLocaleTimeString();
    elements.lastUpdateTime.title = now.toLocaleString();
}

// fonction pour gérer la réinitialisation
function handleDayReset(date) {
    addSystemMessage(`Réinitialisation quotidienne pour le ${date}`, "info");
    
    const rows = elements.fingerprintsTable.querySelectorAll('tbody tr');
    for (const row of rows) {
        if (row.cells[0].textContent) {
            // Réinitialiser les heures
            row.cells[2].textContent = "-";
            row.cells[3].textContent = "-";
            
            // Mettre à jour le statut
            const statusCell = row.cells[4];
            statusCell.textContent = "absent";
            statusCell.className = "status-absent";
        }
    }
    
    // Afficher une notification
    showAlert("Nouvelle Journée", `Pointages réinitialisés pour le ${date}`, "info", 5000);
}

// Fonction pour supprimer une ligne
function removeEmployeeFromTable(id) {
    const rows = elements.fingerprintsTable.querySelectorAll('tbody tr');
    
    for (const row of rows) {
        if (row.cells[0].textContent == id) {
            row.classList.add('fade-out');
            setTimeout(() => row.remove(), 500);
            break;
        }
    }
    
    // Si la table est vide après suppression
    if (elements.fingerprintsTable.querySelectorAll('tbody tr').length === 0) {
        const row = elements.fingerprintsTable.querySelector('tbody').insertRow();
        const cell = row.insertCell(0);
        cell.colSpan = 5;
        cell.textContent = "Aucune empreinte enregistrée";
        cell.className = "message-chargement";
        cell.setAttribute('data-label', '');
    }
    
    // Mise à jour de l'heure de dernière mise à jour
    const now = new Date();
    elements.lastUpdateTime.textContent = now.toLocaleTimeString();
    elements.lastUpdateTime.title = now.toLocaleString();
}

function getPresenceStatus(checkIn, checkOut) {
    if (checkIn && !checkOut) return "present";
    if (checkIn && checkOut) return "left";
    return "absent";
}

function highlightEmployeeRow(id) {
    const rows = elements.fingerprintsTable.querySelectorAll('tbody tr');
    
    for (const row of rows) {
        if (row.cells[0].textContent == id) {
            row.classList.add("highlight");
            setTimeout(() => row.classList.remove("highlight"), 3000);
            break;
        }
    }
}

// Fonctions utilitaires
function addSystemMessage(message, type = "info") {
    // Limite à 20 messages
    if (elements.systemMessages.children.length >= 20) {
        elements.systemMessages.removeChild(elements.systemMessages.lastChild);
    }
    
    // Ajout avec horodatage
    const p = document.createElement('p');
    p.className = `message-${type}`;
    p.innerHTML = `<span class="timestamp">[${new Date().toLocaleTimeString()}]</span> ${message}`;
    
    elements.systemMessages.prepend(p);
}

function updateStatus(cpuStatus, sensorStatus, timeStatus) {
    if (cpuStatus !== null) {
        elements.statutESP32.textContent = cpuStatus;
        updateStatusClass(elements.statutESP32.parentElement, cpuStatus);
    }
    
    if (sensorStatus !== null) {
        elements.statutCapteur.textContent = sensorStatus;
        updateStatusClass(elements.statutCapteur.parentElement, sensorStatus);
    }
    
    if (timeStatus !== null) {
        elements.statutHeure.textContent = timeStatus;
        updateStatusClass(elements.statutHeure.parentElement, timeStatus);
    }
}

function updateStatusClass(element, status) {
    // Réinitialisation des classes
    element.classList.remove("status-error", "status-warning", "status-success");
    
    if (status.includes("Erreur") || status.includes("Déconnecté")) {
        element.classList.add("status-error");
    } else if (status.includes("Connexion") || status.includes("Sync")) {
        element.classList.add("status-warning");
    } else {
        element.classList.add("status-success");
    }
}

function showAlert(title, message, type = "info", duration = 5000) {
    // Réinitialisation CSS
    elements.modalTitle.className = "";
    elements.modalTitle.classList.add(type);

    elements.modalTitle.textContent = title;
    elements.modalMessage.textContent = message;
    elements.alertModal.style.display = "block";
    
    // Réinitialiser et relancer l'animation
    const progressBar = document.querySelector(".progress");
    progressBar.style.animation = "none";
    void progressBar.offsetWidth; // Forcer le reflow
    progressBar.style.animation = `progressAnimation ${duration/1000}s linear forwards`;
    
    // Fermeture automatique
    setTimeout(closeModal, duration);
}

function closeModal() {
    elements.alertModal.style.display = "none";
}

// Fonction pour mettre le bouton en état de chargement
function setButtonLoading(button) {
    button.dataset.originalText = button.innerHTML;
    button.innerHTML = '<span class="spinner"></span> ' + button.dataset.originalText;
    button.disabled = true;
    button.classList.add('button-loading');
}

// Fonction pour réinitialiser le bouton
function resetButton(button) {
    button.innerHTML = button.dataset.originalText;
    button.disabled = false;
    button.classList.remove('button-loading');
}

// Démarrer l'application
window.addEventListener("load", initApplication);
