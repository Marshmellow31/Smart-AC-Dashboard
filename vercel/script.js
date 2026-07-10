import { initializeApp } from "https://www.gstatic.com/firebasejs/10.9.0/firebase-app.js";
import { getAuth, signInWithPopup, GoogleAuthProvider, onAuthStateChanged, signOut } from "https://www.gstatic.com/firebasejs/10.9.0/firebase-auth.js";
import { getDatabase, ref, onValue, set, update } from "https://www.gstatic.com/firebasejs/10.9.0/firebase-database.js";

// The config you provided:
const firebaseConfig = {
  apiKey: "AIzaSyCI4QK9dSxx1UD6399cQ-lRwDTZGMmzP9M",
  authDomain: "ac-controller-430f7.firebaseapp.com",
  databaseURL: "https://ac-controller-430f7-default-rtdb.firebaseio.com",
  projectId: "ac-controller-430f7",
  storageBucket: "ac-controller-430f7.firebasestorage.app",
  messagingSenderId: "512769029119",
  appId: "1:512769029119:web:4fa13e93a9177ddb2cedca",
  measurementId: "G-5RKXCD1JE1"
};

// Initialize Firebase
const app = initializeApp(firebaseConfig);
const auth = getAuth(app);
const db = getDatabase(app);

// DOM Elements
const loginOverlay = document.getElementById("loginOverlay");
const appShell = document.getElementById("appShell");
const googleSignInBtn = document.getElementById("googleSignInBtn");
const signOutBtn = document.getElementById("signOutBtn");
const connDot = document.getElementById("connDot");

const powerToggle = document.getElementById("powerToggle");
const powerState = document.getElementById("powerState");
const tempDown = document.getElementById("tempDown");
const tempUp = document.getElementById("tempUp");
const tempValue = document.getElementById("tempValue");
const modeBtns = document.querySelectorAll(".mode-btn");
const fanBtns = document.querySelectorAll(".chip");

// Current State
let state = {
  power: false,
  mode: "cool",
  temp: 24,
  fan: "auto"
};

// --- AUTHENTICATION ---
const provider = new GoogleAuthProvider();

googleSignInBtn.addEventListener("click", () => {
  signInWithPopup(auth, provider).catch(error => {
    console.error("Login failed", error);
    alert("Login failed: " + error.message);
  });
});

signOutBtn.addEventListener("click", () => {
  signOut(auth);
});

onAuthStateChanged(auth, (user) => {
  if (user) {
    // User is signed in.
    loginOverlay.style.display = "none";
    appShell.style.display = "flex";
    connDot.classList.remove("offline");
    connDot.classList.add("online");
    listenToDatabase();
  } else {
    // User is signed out.
    loginOverlay.style.display = "flex";
    appShell.style.display = "none";
    connDot.classList.remove("online");
    connDot.classList.add("offline");
  }
});

// --- DATABASE SYNC ---
function listenToDatabase() {
  const acRef = ref(db, 'acState');
  onValue(acRef, (snapshot) => {
    const data = snapshot.val();
    if (data) {
      state = { ...state, ...data };
      updateUI();
    }
  });
}

function updateDatabase(partialState) {
  state = { ...state, ...partialState };
  updateUI(); // optimistic update
  
  // Write to Firebase
  const acRef = ref(db, 'acState');
  update(acRef, partialState).catch(err => {
    console.error("Failed to update database", err);
    // In a real app, you might revert the UI here if it fails
  });
}

// --- UI UPDATES ---
function updateUI() {
  // Power
  if (state.power) {
    powerToggle.setAttribute("aria-checked", "true");
    powerState.innerText = "On";
  } else {
    powerToggle.setAttribute("aria-checked", "false");
    powerState.innerText = "Off";
  }

  // Temp
  tempValue.innerText = state.temp;

  // Mode
  modeBtns.forEach(btn => {
    btn.setAttribute("aria-pressed", btn.dataset.mode === state.mode ? "true" : "false");
  });

  // Fan
  fanBtns.forEach(btn => {
    btn.setAttribute("aria-pressed", btn.dataset.fan === state.fan ? "true" : "false");
  });
}

// --- EVENT LISTENERS ---
powerToggle.addEventListener("click", () => {
  updateDatabase({ power: !state.power });
});

tempDown.addEventListener("click", () => {
  if (state.temp > 16) updateDatabase({ temp: state.temp - 1 });
});

tempUp.addEventListener("click", () => {
  if (state.temp < 30) updateDatabase({ temp: state.temp + 1 });
});

modeBtns.forEach(btn => {
  btn.addEventListener("click", () => {
    updateDatabase({ mode: btn.dataset.mode, power: true }); // Switching mode usually turns AC on
  });
});

fanBtns.forEach(btn => {
  btn.addEventListener("click", () => {
    updateDatabase({ fan: btn.dataset.fan });
  });
});
