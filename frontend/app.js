// ============================================
//   Smart Inventory Dashboard — app.js
//   Struktur Firebase disesuaikan dengan backend
// ============================================

// -----------------------------------------------
// 1. KONFIGURASI FIREBASE
// -----------------------------------------------
const firebaseConfig = {
  apiKey:            "AIzaSyAijPXyQWz4LnmCTujlnENTWMxEGdwmANg",
  authDomain:        "smart-inventory-iot-407f4.firebaseapp.com",
  databaseURL:       "https://smart-inventory-iot-407f4-default-rtdb.asia-southeast1.firebasedatabase.app/",
  projectId:         "smart-inventory-iot-407f4",
  storageBucket:     "smart-inventory-iot-407f4.firebasestorage.app",
  messagingSenderId: "1025430275478",
  appId:             "1:1025430275478:web:ab6c714a0c806733ef89b7"
};

// -----------------------------------------------
// 2. INISIALISASI FIREBASE
// -----------------------------------------------
firebase.initializeApp(firebaseConfig);
const db = firebase.database();

// -----------------------------------------------
// 3. STATE LOKAL
// -----------------------------------------------
let state    = {};  // { barang_001: { id, nama_barang, stok_sekarang, ambang_batas, status_alert } }
let totalIn  = 0;
let totalOut = 0;

// -----------------------------------------------
// 4. LISTENER INVENTORY
//    Baca dari /inventory — update otomatis saat ESP32 ubah data
// -----------------------------------------------
function listenInventory() {
  db.ref("inventory").on("value", (snapshot) => {
    setStatus("connected");

    const data = snapshot.val();
    if (!data) return;

    state = {};
    Object.entries(data).forEach(([id, item]) => {
      state[id] = {
        id,
        nama_barang:    item.nama_barang    ?? "Barang",
        stok_sekarang:  item.stok_sekarang  ?? 0,
        ambang_batas:   item.ambang_batas   ?? 3,
        status_alert:   item.status_alert   ?? false,
      };
    });

    renderCards();
  }, (err) => {
    console.error("Firebase inventory error:", err);
    setStatus("error");
  });
}

// -----------------------------------------------
// 5. LISTENER LOG TRANSAKSI
//    Baca dari /log_transaksi — tampil di log aktivitas
//    orderByChild timestamp, ambil 20 terakhir
// -----------------------------------------------
function listenLog() {
  db.ref("log_transaksi").orderByChild("timestamp").limitToLast(20).on("value", (snapshot) => {
    const data = snapshot.val();
    if (!data) return;

    // Balik urutan supaya terbaru di atas
    const entries = Object.values(data).reverse();
    renderLog(entries);
  });
}

// -----------------------------------------------
// 6. LISTENER NOTIFIKASI ALERT
//    Baca dari /notifikasi_alert — tampil badge unread
// -----------------------------------------------
function listenNotifikasi() {
  db.ref("notifikasi_alert").orderByChild("status").equalTo("unread").on("value", (snapshot) => {
    const data = snapshot.val();
    const count = data ? Object.keys(data).length : 0;
    renderNotifBadge(count);
  });
}

// -----------------------------------------------
// 7. UPDATE STOK DARI DASHBOARD
//    Tulis ke /inventory dan tambah log transaksi
// -----------------------------------------------
function updateStock(id, tipe) {
  const item = state[id];
  if (!item) return;

  const delta    = tipe === "IN" ? 1 : -1;
  const newStock = item.stok_sekarang + delta;
  if (newStock < 0) return;

  const now       = Math.floor(Date.now() / 1000);
  const isAlert   = newStock <= item.ambang_batas;

  // Update stok + status alert di inventory
  db.ref("inventory/" + id).update({
    stok_sekarang:  newStock,
    status_alert:   isAlert,
    terakhir_diubah: now,
  });

  // Tulis log transaksi baru
  db.ref("log_transaksi").push({
    id_barang: id,
    tipe,
    jumlah:    1,
    timestamp: now,
    metode:    "dashboard",
  });

  // Kalau stok kritis, tulis notifikasi
  if (isAlert) {
    db.ref("notifikasi_alert").push({
      id_barang: id,
      pesan:     `Peringatan! Stok ${item.nama_barang} kritis (kurang dari ${item.ambang_batas}).`,
      timestamp: now,
      status:    "unread",
    });
  }

  // Update counter summary
  if (tipe === "IN") totalIn++;
  else totalOut++;
  renderSummary();
}

// -----------------------------------------------
// 8. RENDER KARTU STOK
// -----------------------------------------------
function renderCards() {
  const grid = document.getElementById("cards-grid");
  grid.innerHTML = "";

  let alertCount = 0;
  const items = Object.values(state);

  if (items.length === 0) {
    grid.innerHTML = '<p class="loading-text">Belum ada data inventory di Firebase...</p>';
    return;
  }

  items.forEach((item) => {
    const isLow = item.status_alert || item.stok_sekarang <= item.ambang_batas;
    if (isLow) alertCount++;

    const card = document.createElement("div");
    card.className = "item-card" + (isLow ? " alert" : "");
    card.id = "card-" + item.id;

    card.innerHTML = `
      <p class="card-name">${item.nama_barang}</p>
      <p class="card-stock ${isLow ? "low" : ""}">${item.stok_sekarang}</p>
      <p class="card-unit">pcs &nbsp;|&nbsp; batas: ${item.ambang_batas}</p>
      ${isLow ? `<div class="alert-badge">&#9888; Restock required</div>` : ""}
      <div class="card-actions">
        <button class="btn-action in" >+ Masuk</button>
        <button class="btn-action out"  ${item.stok_sekarang <= 0 ? "disabled" : ""}>- Keluar</button>
      </div>
    `;

    grid.appendChild(card);
  });

  document.getElementById("total-alert").textContent = alertCount;
}

// -----------------------------------------------
// 9. RENDER LOG TRANSAKSI
// -----------------------------------------------
function renderLog(entries) {
  const list = document.getElementById("log-list");

  if (!entries || entries.length === 0) {
    list.innerHTML = '<li class="log-empty">Belum ada transaksi...</li>';
    return;
  }

  list.innerHTML = entries.map((l) => {
    const isIn   = l.tipe === "IN";
    const nama   = state[l.id_barang]?.nama_barang ?? l.id_barang;
    const time   = l.timestamp ? new Date(l.timestamp * 1000).toLocaleTimeString("id-ID", { hour: "2-digit", minute: "2-digit" }) : "--:--";
    const metode = l.metode ? `<span class="log-metode">${l.metode}</span>` : "";
    return `
      <li class="log-item">
        <span class="log-arrow ${isIn ? "in" : "out"}">${isIn ? "↑" : "↓"}</span>
        <span class="log-item-name">${nama}</span>
        <span class="log-badge ${isIn ? "in" : "out"}">${isIn ? "+" : "-"}${l.jumlah} ${isIn ? "masuk" : "keluar"}</span>
        ${metode}
        <span class="log-time">${time}</span>
      </li>
    `;
  }).join("");
}

// -----------------------------------------------
// 10. RENDER SUMMARY
// -----------------------------------------------
function renderSummary() {
  document.getElementById("total-in").textContent  = totalIn;
  document.getElementById("total-out").textContent = totalOut;
}

// -----------------------------------------------
// 11. RENDER NOTIF BADGE
// -----------------------------------------------
function renderNotifBadge(count) {
  const badge = document.getElementById("notif-badge");
  if (!badge) return;
  badge.textContent = count;
  badge.style.display = count > 0 ? "inline-block" : "none";
}

// -----------------------------------------------
// 12. TANDAI SEMUA NOTIF SUDAH DIBACA
// -----------------------------------------------
function clearNotifikasi() {
  db.ref("notifikasi_alert").orderByChild("status").equalTo("unread").once("value", (snapshot) => {
    const data = snapshot.val();
    if (!data) return;
    const updates = {};
    Object.keys(data).forEach((key) => {
      updates[key + "/status"] = "read";
    });
    db.ref("notifikasi_alert").update(updates);
  });
}

// -----------------------------------------------
// 13. STATUS INDIKATOR
// -----------------------------------------------
function setStatus(status) {
  const dot  = document.querySelector(".status-dot");
  const text = document.getElementById("status-text");
  dot.className = "status-dot " + status;
  if (status === "connected")    text.textContent = "Live";
  else if (status === "error")   text.textContent = "Gagal terhubung";
  else                           text.textContent = "Menghubungkan...";
}

// -----------------------------------------------
// 14. MULAI SEMUA LISTENER
// -----------------------------------------------
listenInventory();
listenLog();
listenNotifikasi();