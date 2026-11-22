// Play of the Game tab for the League client (Pengu Loader plugin). Lists your recent games from the client, asks the
// POTG helper (main.exe, http://127.0.0.1:47811) for each game's best plays and how they scored, and has it watch or save one
const HELPER = "http://127.0.0.1:47811";
let SONGS = [["", "Random song"]];
const state = { games: [], champions: {}, queues: {}, spells: {}, items: {}, maps: [], selected: null, plays: {}, song: "", status: null, helperUp: false, open: false,
    older: [], olderFrom: 0, noOlder: false }; // older: games past the client's latest 20; olderFrom: how far into Riot's list we are

const STYLE = `
#potg-panel { position: fixed; z-index: 99000; display: none; flex-direction: column; background: linear-gradient(#010a13ee, #010a13f8),
  radial-gradient(circle at 70% 0%, #0a323c, #010a13 60%); border-top: 1px solid #785a28; color: #a09b8c; font-family: var(--font-body, "LoL Body"), sans-serif; font-size: 12px; }
#potg-panel.open { display: flex; }
#potg-panel h1, #potg-panel h2, #potg-panel .potg-display { font-family: var(--font-display, "LoL Display"), serif; text-transform: uppercase; letter-spacing: .08em; margin: 0; }
#potg-panel h1 { color: #f0e6d2; font-size: 22px; font-weight: 700; }
#potg-panel h2 { color: #c8aa6e; font-size: 12px; font-weight: 700; margin-bottom: 8px; }
.potg-head { display: flex; align-items: center; gap: 16px; padding: 16px 24px 10px; border-bottom: 1px solid #1e2328; }
.potg-head .potg-spacer { flex: 1; }
.potg-body { flex: 1; display: flex; min-height: 0; }
.potg-games { width: 360px; overflow-y: auto; padding: 12px 8px 12px 24px; border-right: 1px solid #1e2328; }
.potg-game { display: grid; grid-template-columns: 52px 1fr auto; column-gap: 10px; padding: 10px 8px; cursor: pointer; border: 1px solid transparent;
  border-bottom-color: #1e2328; }
.potg-game:hover { background: #1e232880; }
.potg-game.selected { border-color: #785a28; background: linear-gradient(90deg, #785a2840, transparent); }
.potg-portrait { position: relative; width: 48px; height: 48px; grid-row: span 2; }
.potg-portrait img { width: 48px; height: 48px; border-radius: 50%; border: 2px solid #785a28; box-sizing: border-box; }
.potg-lv { position: absolute; right: -4px; bottom: -2px; min-width: 18px; height: 18px; line-height: 18px; text-align: center; border-radius: 50%;
  background: #010a13; border: 1px solid #785a28; color: #a09b8c; font: 700 10px var(--font-display, "LoL Display"), serif; }
.potg-result { font: 700 14px var(--font-display, "LoL Display"), serif; letter-spacing: 1.05px; text-transform: uppercase; }
.potg-result.win { color: #0acbe6; } .potg-result.loss { color: #ff2345; }
.potg-mode, .potg-where { color: #a09b8c; font-size: 12px; letter-spacing: .3px; }
.potg-where { text-align: right; line-height: 1.6; }
.potg-bullet { color: #3c3c41; margin: 0 4px; }
.potg-icons { display: flex; gap: 2px; margin-top: 4px; }
.potg-icons img { width: 18px; height: 18px; border: 1px solid #3c3c41; }
.potg-icons span { width: 18px; height: 18px; border: 1px solid #1e2328; background: #010a13; }
.potg-stats { grid-column: 2 / 4; display: flex; flex-wrap: wrap; align-items: center; column-gap: 14px; margin-top: 6px; color: #a09b8c;
  font: 700 14px var(--font-display, "LoL Display"), serif; letter-spacing: .8px; }
.potg-stats .potg-items { margin: 0 0 4px; flex-basis: 100%; }
.potg-stats i { display: inline-block; width: 13px; height: 14px; margin-left: 3px; vertical-align: -1px; background: no-repeat center / contain; }
.potg-stats i.cs { background-image: url(/fe/lol-match-history/icon_minions.png); }
.potg-stats i.gold { width: 16px; height: 13px; background-image: url(/fe/lol-match-history/icon_gold.png); }
.potg-win { color: #0acbe6; } .potg-loss { color: #e84057; }
.potg-older { display: block; width: 100%; margin: 12px 0 4px; }
.potg-sub { color: #5b5a56; font-size: 11px; }
.potg-plays { flex: 1; overflow-y: auto; padding: 12px 24px; }
.potg-note { color: #a09b8c; margin-bottom: 12px; line-height: 1.5; }
.potg-play { border: 1px solid #463714; background: #010a13cc; margin-bottom: 12px; padding: 12px 14px; }
.potg-play.first { border-color: #c8aa6e; box-shadow: 0 0 12px #c8aa6e30; }
.potg-play-head { display: flex; align-items: center; gap: 10px; }
.potg-play-head img { width: 44px; height: 44px; border-radius: 50%; border: 2px solid #c8aa6e; }
.potg-play-head img.champ { border-radius: 0; border: 1px solid #785a28; }
.potg-who { flex: 1; }
.potg-who .potg-display { color: #f0e6d2; font-size: 16px; font-weight: 700; }
.potg-score { color: #f0e6d2; font-size: 24px; font-weight: 700; text-align: right; }
.potg-score small { color: #c8aa6e; font-size: 11px; margin-left: 4px; }
.potg-events { width: 100%; border-collapse: collapse; margin: 10px 0; }
.potg-events td { padding: 3px 8px 3px 0; vertical-align: top; border-top: 1px solid #1e2328; }
.potg-events th { color: #5b5a56; font-weight: 400; text-align: left; padding: 0 8px 4px 0; }
.potg-events .t { color: #5b5a56; width: 44px; } .potg-events .l { color: #f0e6d2; width: 210px; }
.potg-events .p { color: #a09b8c; line-height: 1.5; } .potg-events .p b { display: block; color: #c8aa6e; font-size: 14px; }
.potg-btn { font-family: var(--font-display, "LoL Display"), serif; text-transform: uppercase; letter-spacing: .08em; font-weight: 700; font-size: 12px;
  color: #cdbe91; background: #1e2328; border: 2px solid #785a28; padding: 6px 18px; margin-right: 8px; cursor: pointer; }
.potg-btn:hover { color: #f0e6d2; border-color: #c8aa6e; background: linear-gradient(#1e2328, #2c2a22); }
.potg-btn:disabled { color: #5b5a56; border-color: #3c3c41; cursor: default; background: #1e2328; }
.potg-select, .potg-input { background: #010a13; color: #f0e6d2; border: 1px solid #785a28; padding: 5px 8px; font-family: inherit; font-size: 12px; }
.potg-status { padding: 8px 24px; border-top: 1px solid #1e2328; color: #a09b8c; min-height: 18px; }
.potg-status.err { color: #e84057; }
#potg-tab { cursor: pointer; pointer-events: auto; color: #cdbe91; font-family: var(--font-display, "LoL Display"), serif; font-size: 12px; font-weight: 500; letter-spacing: 1.2px; text-transform: uppercase; }
#potg-tab.potg-fallback { position: fixed; top: 8px; left: 50%; transform: translateX(-50%); z-index: 99001; color: #cdbe91; background: #1e2328;
  border: 1px solid #785a28; padding: 4px 12px; font-family: var(--font-display, "LoL Display"), serif; font-weight: 700; letter-spacing: .08em; }
#potg-tab.active, #potg-tab:hover { color: #f0e6d2; }
`;

const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const clock = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;
const championIcon = (id) => `/lol-game-data/assets/v1/champion-icons/${id}.png`;
const profileIcon = (id) => `/lol-game-data/assets/v1/profile-icons/${id}.jpg`;

async function lcu(path) {
    const r = await fetch(path);
    if (!r.ok) throw new Error(`${path}: ${r.status}`);
    return r.json();
}

async function helper(path, body) {
    const r = await fetch(HELPER + path, body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {});
    const data = await r.json();
    if (!r.ok) throw new Error(data.error || r.status);
    return data;
}

// The map as match history names it: a map skin (Koeshin's Crossing) wins over the plain map for the mode
function mapName(g) {
    const mutators = (g.gameModeMutators ?? []).map((m) => m.toLowerCase());
    const maps = state.maps.filter((m) => m.id === g.mapId);
    const skin = maps.find((m) => m.gameMutator && mutators.includes(m.gameMutator.toLowerCase()));
    return (skin ?? maps.find((m) => m.gameMode === g.gameMode) ?? maps[0])?.name ?? "";
}

async function loadGames() {
    if (!Object.keys(state.champions).length) {
        const list = (path) => lcu(path).catch(() => []);
        const [champions, queues, spells, items, maps] = await Promise.all([lcu("/lol-game-data/assets/v1/champion-summary.json"),
            list("/lol-game-queues/v1/queues"), list("/lol-game-data/assets/v1/summoner-spells.json"),
            list("/lol-game-data/assets/v1/items.json"), list("/lol-maps/v2/maps")]);
        for (const c of champions) state.champions[c.id] = c.name;
        for (const q of queues) state.queues[q.id] = q.detailedDescription || q.name; // "Normal (Draft Pick)", as match history says it
        for (const sp of spells) state.spells[sp.id] = sp.iconPath;
        for (const it of items) state.items[it.id] = it.iconPath;
        state.maps = maps;
    }
    // The client only hands out your latest 20 games; older ones come from loadOlder
    const history = await lcu("/lol-match-history/v1/products/lol/current-summoner/matches?begIndex=0&endIndex=19");
    const seen = new Set();
    const raw = [...(history.games?.games ?? []), ...state.older].filter((g) => !seen.has(g.gameId) && seen.add(g.gameId));
    state.games = raw.map((g) => {
        const p = g.participants?.[0] ?? {}, s = p.stats ?? {};
        return {
            id: g.gameId, champion: p.championId ?? 0, win: !!s.win, level: s.champLevel ?? 0,
            queue: state.queues[g.queueId] || g.gameMode, map: mapName(g), length: g.gameDuration, date: new Date(g.gameCreation),
            kda: `${s.kills ?? 0} / ${s.deaths ?? 0} / ${s.assists ?? 0}`, cs: (s.totalMinionsKilled ?? 0) + (s.neutralMinionsKilled ?? 0),
            gold: s.goldEarned ?? 0, spells: [p.spell1Id, p.spell2Id], items: [0, 1, 2, 3, 4, 5, 6].map((i) => s[`item${i}`]),
        };
    });
    render();
    if (!state.selected && state.games.length) selectGame(state.games[0].id);
}

// The next 20 older games: their IDs from the helper (Riot's API), then each game's details from the client, trimmed to
// your own row the way the client's match history gives it
async function loadOlder() {
    // Riot's list and the client's don't line up (Riot leaves some modes out), so walk Riot's from the start and skip
    // games already shown, until some new ones turn up
    let games = [];
    while (!games.length) {
        const page = (await helper(`/api/older?start=${state.olderFrom}&count=20`)).games;
        state.olderFrom += 20;
        if (!page.length) { state.noOlder = true; break; }
        games = page.filter((id) => !state.games.some((g) => g.id === id));
    }
    const me = await lcu("/lol-summoner/v1/current-summoner");
    for (const id of games) {
        const game = await lcu(`/lol-match-history/v1/games/${id}`).catch(() => null);
        const mine = game?.participantIdentities?.find((i) => i.player?.puuid === me.puuid);
        if (!mine) continue;
        game.participants = game.participants.filter((p) => p.participantId === mine.participantId);
        state.older.push(game);
    }
    await loadGames();
}

async function selectGame(id) {
    state.selected = id;
    render();
    try {
        state.plays[id] = await helper(`/api/plays?game=${id}`);
        state.helperUp = true;
    } catch (e) {
        state.plays[id] = { error: e instanceof TypeError ? null : e.message };
        state.helperUp = !(e instanceof TypeError);
    }
    render();
}

async function show(play, save) {
    try {
        await helper("/api/show", { game: state.selected, player: play.player, start: play.start, end: play.end, music: state.song, save });
        state.returnTo = true; // the replay takes the client's view; come back to the tab when it's over
        state.started = true;
        poll();
    } catch (e) {
        state.status = { error: e instanceof TypeError ? "The POTG helper isn't running. Start main.exe." : e.message };
        render();
    }
}

// Has the helper read the game's replay, which picks its play of the game, and play it: intro, then the scene. The
// card with who and why only shows once it has played
async function find() {
    try {
        await helper("/api/find", { game: state.selected, music: state.song });
        state.started = true;
        state.returnTo = true;
        poll();
    } catch (e) {
        state.status = { error: e instanceof TypeError ? "The POTG helper isn't running. Start main.exe." : e.message };
        render();
    }
}

async function saveKey() {
    const input = document.querySelector("#potg-key");
    await helper("/api/key", { key: input.value.trim() });
    state.plays = {};
    if (state.selected) selectGame(state.selected);
}

let polling = false;
async function poll() {
    if (polling) return;
    polling = true;
    try {
        for (;;) {
            try {
                state.status = await helper("/api/status");
                state.helperUp = true;
                if (SONGS.length == 1){
                    SONGS = [["", "Random song"], ...(await helper("/api/music")).songs.map((s) => [s.key,s.name])];
                }
            } catch {
                state.helperUp = false;
                state.status = null;
            }
            render();
            if (!state.status?.busy) break;
            await new Promise((r) => setTimeout(r, 1000));
        }
        if (state.returnTo) {
            state.returnTo = false;
            if (!state.open) setOpen(true);
        }
        // Finding a play reads the replay, after which the game has one
        if (state.selected && state.helperUp) {
            delete state.plays[state.selected];
            selectGame(state.selected);
        }
    } finally {
        polling = false;
    }
}

// Laid out like the client's match history: result, queue and spells by the champion, map and time on the right, then items,
// KDA, CS and gold
function gameList() {
    const icon = (path) => (path ? `<img src="${esc(path)}">` : "<span></span>");
    return state.games.map((g) => `
        <div class="potg-game ${g.id === state.selected ? "selected" : ""}" data-game="${g.id}">
            <div class="potg-portrait"><img src="${championIcon(g.champion)}"><span class="potg-lv">${g.level}</span></div>
            <div>
                <div class="potg-result ${g.win ? "win" : "loss"}">${g.win ? "Victory" : "Defeat"}</div>
                <div class="potg-mode">${esc(g.queue)}</div>
                <div class="potg-icons">${g.spells.map((id) => icon(state.spells[id])).join("")}</div>
            </div>
            <div class="potg-where">${esc(g.map)}<br>${clock(g.length)}<span class="potg-bullet">&bull;</span>${g.date.toLocaleDateString()}</div>
            <div class="potg-stats">
                <div class="potg-icons potg-items">${g.items.map((id) => icon(id ? state.items[id] : null)).join("")}</div>
                <span>${g.kda}</span><span>${g.cs}<i class="cs"></i></span><span>${g.gold.toLocaleString()}<i class="gold"></i></span>
            </div>
        </div>`).join("");
}

// "Killed Illaoi (quadra kill)" over the player's name, the way a kill feed reads
function eventLabel(e) {
    const champion = state.champions[e.victim_champion];
    if (!e.victim || !champion) return esc(e.label);
    return `${esc(e.label.replace(e.victim, champion))}<div class="potg-sub">${esc(e.victim)}</div>`;
}

function playList() {
    const result = state.plays[state.selected];
    if (!state.selected) return "";
    if (!result) return `<div class="potg-note">Checking the game...</div>`;
    if (result.error === null) return `<div class="potg-note">The POTG helper isn't running. Start <b>main.exe</b> in the POTG_League build folder, then pick the game again.</div>`;
    if (result.error) return `<div class="potg-note potg-loss">${esc(result.error)}</div>`;
    const busy = !!state.status?.busy;
    if (!result.found) {
        return `
        <div class="potg-note">The play of the game comes from the game's replay. Finding it opens the replay, then plays the
            intro and the play itself; who it was and how it scored show here after.</div>
        ${busy ? `<button class="potg-btn" id="potg-stop">Stop</button>` : `<button class="potg-btn" id="potg-find">Find play of the game</button>`}`;
    }
    const p = result.plays[0];
    if (!p) return `<div class="potg-note">Nobody scored in this game.</div>`;
    const card = `
        <div class="potg-play first">
            <div class="potg-play-head">
                <img src="${profileIcon(p.profile_icon)}"><img class="champ" src="${championIcon(p.champion_id)}">
                <div class="potg-who">
                    <div class="potg-display">${esc(p.player)}</div>
                    <div class="potg-sub">as ${esc(state.champions[p.champion_id] ?? "")} &middot; ${clock(p.start)}&ndash;${clock(p.end)}</div>
                </div>
                <div class="potg-score potg-display">${Math.round(p.score)}<small>pts</small></div>
            </div>
            <table class="potg-events">
                <tr><th>Time</th><th>What happened</th><th>Points</th></tr>${p.events.map((e) => `
                <tr><td class="t">${clock(e.time)}</td><td class="l">${eventLabel(e)}</td>
                    <td class="p"><b>${Math.round(e.points)}</b>${e.why.split(String.fromCharCode(10)).map((w) => `<div>${esc(w)}</div>`).join("")}</td></tr>`).join("")}
            </table>
            ${busy ? `<button class="potg-btn" id="potg-stop">Stop</button>` : `
            <button class="potg-btn" data-show="0">Watch</button>
            <button class="potg-btn" data-save="0">Save video</button>`}
        </div>`;
    return card;
}

function statusLine() {
    const s = state.status;
    if (!s || (!s.busy && !state.started)) return { text: "", err: false }; // the helper's last word is from a play started before this
    if (s.error) return { text: s.error, err: true };
    if (s.saved && !s.busy) return { text: `Saved: ${s.saved} <a href="#" id="potg-reveal">Show in folder</a>`, err: false };
    if (!s.busy) return { text: "", err: false };
    return { text: `${esc(s.message)} (Esc stops it)`, err: false };
}

function render() {
    const panel = document.querySelector("#potg-panel");
    if (!panel || !state.open) return;
    const key = state.plays[state.selected]?.riot_key ?? state.status?.riot_key;
    const status = statusLine();
    const scroll = panel.querySelector(".potg-plays")?.scrollTop ?? 0, listScroll = panel.querySelector(".potg-games")?.scrollTop ?? 0;
    panel.innerHTML = `
        <div class="potg-head">
            <h1>Play of the Game</h1>
            <div class="potg-spacer"></div>
            <select class="potg-select" id="potg-song">${SONGS.map(([v, n]) => `<option value="${v}" ${v === state.song ? "selected" : ""}>${n}</option>`).join("")}</select>
            <input class="potg-input" id="potg-key" type="password" placeholder="${key ? "Riot API key saved" : "Riot API key (RGAPI-...)"}" size="22">
            <button class="potg-btn" id="potg-key-save">Save key</button>
        </div>
        <div class="potg-body">
            <div class="potg-games"><h2>Recent games</h2>${gameList()}
                ${state.noOlder ? "" : `<button class="potg-btn potg-older" id="potg-older">Show older games</button>`}</div>
            <div class="potg-plays"><h2>Play of the game</h2>${playList()}</div>
        </div>
        <div class="potg-status ${status.err ? "err" : ""}">${status.text}</div>`;
    panel.querySelector(".potg-plays").scrollTop = scroll;
    panel.querySelector(".potg-games").scrollTop = listScroll; // redrawing mustn't jump the list back to the top
    panel.querySelectorAll("[data-game]").forEach((el) => el.addEventListener("click", () => selectGame(Number(el.dataset.game))));
    const plays = state.plays[state.selected]?.plays ?? [];
    panel.querySelectorAll("[data-show]").forEach((el) => el.addEventListener("click", () => show(plays[el.dataset.show], false)));
    panel.querySelectorAll("[data-save]").forEach((el) => el.addEventListener("click", () => show(plays[el.dataset.save], true)));
    panel.querySelector("#potg-song").addEventListener("change", (e) => (state.song = e.target.value));
    panel.querySelector("#potg-key-save").addEventListener("click", saveKey);
    panel.querySelector("#potg-find")?.addEventListener("click", find);
    panel.querySelector("#potg-older")?.addEventListener("click", (e) => {
        e.target.disabled = true;
        e.target.textContent = "Loading older games...";
        loadOlder().catch((err) => { state.status = { error: `Couldn't load older games: ${err.message}` }; render(); });
    });
    panel.querySelector("#potg-stop")?.addEventListener("click", () => helper("/api/stop", {}).catch(() => {}));
    panel.querySelector("#potg-reveal")?.addEventListener("click", (e) => { e.preventDefault(); helper("/api/reveal", {}); });
}

// The panel covers the client's main view, sub-tabs included: from the bottom of the top bar, left of the friends list
function place(panel) {
    const bar = tab && !tab.classList.contains("potg-fallback") ? tab : document.querySelector(".main-navigation-menu-item");
    const social = document.querySelector(".lol-social-sidebar, lol-social-roster, .social-sidebar");
    const top = bar ? Math.round(bar.getBoundingClientRect().bottom) : 80;
    const right = social ? Math.round(window.innerWidth - social.getBoundingClientRect().left) : 0;
    Object.assign(panel.style, { top: `${top > 0 && top < 200 ? top : 80}px`, left: "0px", right: `${right > 0 && right < 400 ? right : 0}px`, bottom: "0px" });
}

// While the panel is up only our tab looks selected: the client's selected tab loses its pointer and glow. Looks only;
// the client's own state is left alone
const QUIET = `.section.active { background-image: none !important; color: #cdbe91 !important; }
.section.active::before, .section.active::after { display: none !important; }`;
function quietClientTabs(quiet) {
    for (const item of document.querySelectorAll("lol-uikit-navigation-item.main-navigation-menu-item")) {
        const root = item.shadowRoot;
        if (!root) continue;
        root.querySelector("style.potg-quiet")?.remove();
        if (!quiet) continue;
        const style = document.createElement("style");
        style.className = "potg-quiet";
        style.textContent = QUIET;
        root.appendChild(style);
    }
}

function setOpen(open) {
    state.open = open;
    const panel = document.querySelector("#potg-panel");
    panel.classList.toggle("open", open);
    document.querySelector("#potg-tab")?.classList.toggle("active", open);
    document.querySelector("#potg-tab")?.shadowRoot?.querySelector(".section")?.classList.toggle("active", open);
    quietClientTabs(open);
    if (!open) return;
    place(panel);
    render();
    loadGames().catch((e) => { state.status = { error: `Couldn't load your games: ${e.message}` }; render(); });
    poll();
}

// Adds the tab next to the client's own (Home, TFT, Collection...); with fallback, a small button at the top if that bar
// never shows up
let tab = null;
function addTab(fallback) {
    if (tab?.isConnected) return true;
    const item = document.querySelector(".left-nav-menu lol-uikit-navigation-item");
    if (!item && !fallback) return false;
    tab = document.createElement("div");
    tab.id = "potg-tab";
    tab.textContent = "Play of the Game";
    if (!item) {
        tab.classList.add("potg-fallback");
        document.body.appendChild(tab);
    } else {
        // Not a navigation item: the bar picks its active tab by index among those, so one more would shift the rest.
        // It borrows their look by copying a tab's shadow DOM, and lights up like they do
        const shadow = tab.attachShadow({ mode: "open" });
        shadow.innerHTML = item.shadowRoot.innerHTML;
        shadow.querySelector(".section").className = "section";
        const glow = shadow.querySelector(".section-glow");
        tab.addEventListener("mousemove", (e) => {
            const half = tab.offsetWidth / 2, x = e.offsetX - half;
            Object.assign(glow.style, { transform: `translateX(${x / 1.5}px)`, opacity: 1.3 - Math.abs(x / half) });
        });
        tab.addEventListener("mouseleave", () => (glow.style.opacity = 0));
        item.parentElement.appendChild(tab);
    }
    tab.addEventListener("click", (e) => { e.stopPropagation(); setOpen(true); });
    return true;
}

// The client takes the view back on any click outside the panel but the friends list (its tabs, sub-tabs, Play), when it
// switches tabs itself, or when a match is found
function closeWhenLeft() {
    const keep = "#potg-panel, #potg-tab, .lol-social-sidebar, lol-social-roster, .social-sidebar, .lol-social-lower-pane-container";
    document.addEventListener("pointerdown", (e) => {
        if (!state.open) return;
        if (e.composedPath().some((el) => el instanceof Element && el.matches(keep))) return;
        state.returnTo = false; // they went somewhere else themselves, so a play finishing doesn't pull them back
        setOpen(false);
    }, true);
    new MutationObserver((changes) => {
        if (!state.open) return;
        for (const c of changes) {
            const el = c.target;
            if (!el.classList?.contains("main-navigation-menu-item")) continue;
            const now = c.attributeName === "class" ? el.className : el.getAttribute(c.attributeName);
            const on = (v) => (c.attributeName === "class" ? /active|selected/i.test(v ?? "") : v !== null && v !== "false");
            if (on(now) && !on(c.oldValue)) setOpen(false); // newly active only
        }
    }).observe(document.body, { subtree: true, attributes: true, attributeOldValue: true, attributeFilter: ["class", "active", "selected"] });
    setInterval(async () => {
        if (!state.open) return;
        const phase = await lcu("/lol-gameflow/v1/gameflow-phase").catch(() => "None");
        if (!["None", "Lobby", "Matchmaking", "WatchInProgress"].includes(phase)) setOpen(false); // a replay is no reason to leave
    }, 2000);
}

export function load() {
    if (document.querySelector("#potg-panel")) return; // already loaded
    const style = document.createElement("style");
    style.textContent = STYLE;
    document.head.appendChild(style);
    const panel = document.createElement("div");
    panel.id = "potg-panel";
    document.body.appendChild(panel);
    window.addEventListener("resize", () => state.open && place(panel));
    closeWhenLeft();

    // The navigation bar shows up a while after the client starts and is rebuilt now and then, so keep the tab in it
    let tries = 0;
    setInterval(() => addTab(++tries > 60), 1000);
}
