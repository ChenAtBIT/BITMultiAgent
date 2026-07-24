const state = {
  agents: [],
  generatedAgents: [],
  agentsById: {},
  toolsByAgentId: {},
  executorTools: [],
  agentToolsError: null,
  runId: null,
  sessionId: null,
  snapshot: null,
  pollTimer: null,
  renderedEventSeq: 0,
};

const eventTypes = [
  "run_created",
  "agents_designed",
  "plan_created",
  "skills_activated",
  "tool_started",
  "tool_completed",
  "tool_failed",
  "node_queued",
  "node_entered",
  "node_exited",
  "node_error",
  "run_done",
  "run_stalled",
  "replan_requested",
  "replan_applied",
  "replan_failed",
  "retry_started",
  "retry_done",
];

const elements = {
  addAgentButton: document.getElementById("addAgentButton"),
  agentModeSelect: document.getElementById("agentModeSelect"),
  agentPicker: document.getElementById("agentPicker"),
  clearEventsButton: document.getElementById("clearEventsButton"),
  connectionStatus: document.getElementById("connectionStatus"),
  dagSvg: document.getElementById("dagSvg"),
  eventList: document.getElementById("eventList"),
  generateAgentsButton: document.getElementById("generateAgentsButton"),
  graphSummary: document.getElementById("graphSummary"),
  maxAgentRoundsInput: document.getElementById("maxAgentRoundsInput"),
  maxDynamicAgentsField: document.getElementById("maxDynamicAgentsField"),
  maxDynamicAgentsInput: document.getElementById("maxDynamicAgentsInput"),
  outputsList: document.getElementById("outputsList"),
  planJson: document.getElementById("planJson"),
  referenceInput: document.getElementById("referenceInput"),
  runButton: document.getElementById("runButton"),
  runIdLabel: document.getElementById("runIdLabel"),
  taskInput: document.getElementById("taskInput"),
};

async function init() {
  await ensureSession();
  const [agentsResponse, settingsResponse, toolsResponse] = await Promise.all([
    fetch("/api/agents"),
    fetch("/api/settings"),
    sessionFetch("/api/agents/tools"),
  ]);
  const data = await agentsResponse.json();
  state.agents = data.agents;
  state.agentsById = Object.fromEntries(state.agents.map((agent) => [agent.id, agent]));
  await applyAgentToolsResponse(toolsResponse);
  if (settingsResponse.ok) {
    applyServerSettings(await settingsResponse.json());
  }
  renderAgentPicker();
  renderGraph();
  renderOutputs();

  elements.runButton.addEventListener("click", startRun);
  elements.addAgentButton.addEventListener("click", addAgent);
  elements.generateAgentsButton.addEventListener("click", () => generateAgentDrafts());
  elements.agentModeSelect.addEventListener("change", renderAgentMode);
  elements.clearEventsButton.addEventListener("click", () => {
    elements.eventList.innerHTML = "";
  });
  renderAgentMode();
}

async function ensureSession() {
  const stored = sessionStorage.getItem("dag_session_id");
  if (stored) {
    const resumed = await fetch(`/api/sessions/${encodeURIComponent(stored)}`);
    if (resumed.ok) {
      state.sessionId = stored;
      return;
    }
    sessionStorage.removeItem("dag_session_id");
  }
  const response = await fetch("/api/sessions", { method: "POST" });
  if (!response.ok) throw new Error("Session creation failed");
  const data = await response.json();
  state.sessionId = data.session_id;
  sessionStorage.setItem("dag_session_id", state.sessionId);
}

function sessionFetch(url, options = {}) {
  const headers = new Headers(options.headers || {});
  headers.set("X-Session-ID", state.sessionId);
  return fetch(url, { ...options, headers });
}

function applyServerSettings(settings) {
  const model = settings.model || "qwen-plus";
  setStatus("idle", `Ready · ${model}`);
}

async function applyAgentToolsResponse(response) {
  if (!response.ok) {
    state.agentToolsError = "Tool list unavailable";
    return;
  }
  try {
    const data = await response.json();
    state.toolsByAgentId = data.agents && typeof data.agents === "object" ? data.agents : {};
    state.executorTools = Array.isArray(data.executor_tools)
      ? data.executor_tools
      : (Array.isArray(data.tools) ? data.tools : []);
    state.agentToolsError = null;
  } catch (error) {
    state.agentToolsError = "Tool list unavailable";
    console.warn("Agent tool list parsing failed", error);
  }
}

function renderAgentPicker() {
  const autoMode = getAgentMode() === "auto";
  const agents = getCurrentAgentPool();

  if (autoMode && !agents.length) {
    elements.agentPicker.innerHTML = `<div class="agent-auto-placeholder">No generated agents / 暂无自动 Agent</div>`;
    return;
  }

  elements.agentPicker.innerHTML = agents
    .map((agent, index) => {
      const tools = getAgentTools(agent);
      const toolList = tools.length
        ? `<ul class="agent-tools-list">${tools
            .map((tool) => {
              const normalizedTool = typeof tool === "string" ? { name: tool } : (tool || {});
              const label = normalizedTool.name || normalizedTool.tool_id || "Unnamed tool";
              const plugin = normalizedTool.plugin_id ? `<small>${escapeHtml(normalizedTool.plugin_id)}</small>` : "";
              const description = normalizedTool.description ? ` title="${escapeHtml(normalizedTool.description)}"` : "";
              return `<li${description}><span>${escapeHtml(label)}</span>${plugin}</li>`;
            })
            .join("")}</ul>`
        : `<div class="agent-tools-empty">${escapeHtml(state.agentToolsError || "No tools available / 暂无可用工具")}</div>`;
      return `
        <article class="agent-toggle" data-agent-index="${index}">
          <div class="agent-toggle-head">
            <label>
              <input class="agent-enabled" type="checkbox" checked />
              <span>${escapeHtml(agent.icon || "A")} ${escapeHtml(agent.name || agent.id)}</span>
            </label>
            <button class="agent-remove-button" type="button" title="Remove agent">×</button>
          </div>
          <div class="agent-edit-grid">
            <label>
              <span>ID</span>
              <input class="agent-id-input" type="text" value="${escapeHtml(agent.id)}" />
            </label>
            <label>
              <span>Icon</span>
              <input class="agent-icon-input" type="text" value="${escapeHtml(agent.icon || "A")}" maxlength="2" />
            </label>
            <label>
              <span>Name</span>
              <input class="agent-name-input" type="text" value="${escapeHtml(agent.name)}" />
            </label>
            <label>
              <span>Role</span>
              <textarea class="agent-role-input" rows="2">${escapeHtml(agent.role)}</textarea>
            </label>
            <label class="agent-reference-field">
              <span>Private Materials / 子资料</span>
              <textarea class="agent-reference-input" rows="3" placeholder="Only this agent can read these materials / 仅当前 Agent 可读取">${escapeHtml((agent.reference_materials || []).join("\\n\\n---\\n\\n"))}</textarea>
            </label>
          </div>
          <section class="agent-tools" aria-label="Tools / 工具">
            <div class="agent-tools-head">
              <span>Tools / 工具</span>
              <strong>${tools.length}</strong>
            </div>
            ${toolList}
          </section>
          <div class="agent-file-row">
            <input class="agent-file-input" type="file" accept=".md,.markdown,.txt" multiple />
            <small class="agent-upload-status">MD, TXT</small>
          </div>
        </article>
      `;
    })
    .join("");

  elements.agentPicker.querySelectorAll(".agent-toggle").forEach((card) => {
    const index = Number(card.dataset.agentIndex);
    const sync = () => syncAgentFromCard(card, index);
    card.querySelectorAll("input, textarea").forEach((input) => {
      input.addEventListener("input", sync);
      input.addEventListener("change", sync);
    });
    card.querySelector(".agent-remove-button").addEventListener("click", () => {
      const pool = getCurrentAgentPool();
      if (pool.length <= 1) return;
      pool.splice(index, 1);
      rebuildAgentsById();
      renderAgentPicker();
    });
    card.querySelector(".agent-file-input").addEventListener("change", () => appendAgentFileMaterials(card, index));
  });
}

function syncAgentFromCard(card, index) {
  const previous = getCurrentAgentPool()[index] || {};
  const agent = {
    ...previous,
    id: normalizeAgentId(card.querySelector(".agent-id-input").value),
    icon: card.querySelector(".agent-icon-input").value.trim().slice(0, 2).toUpperCase() || "A",
    name: card.querySelector(".agent-name-input").value.trim(),
    role: card.querySelector(".agent-role-input").value.trim(),
    reference_materials: getTextareaMaterials(card.querySelector(".agent-reference-input").value),
  };
  agent.name = agent.name || agent.id || "Agent";
  agent.role = agent.role || "General purpose task execution";
  getCurrentAgentPool()[index] = agent;
  rebuildAgentsById();
}

async function appendAgentFileMaterials(card, index) {
  const input = card.querySelector(".agent-file-input");
  const status = card.querySelector(".agent-upload-status");
  const textarea = card.querySelector(".agent-reference-input");
  const files = [...(input.files || [])];
  if (!files.length) return;

  status.textContent = "Parsing...";
  const parsed = [];
  const failed = [];
  for (const file of files) {
    try {
      const contentBase64 = await fileToBase64(file);
      const response = await sessionFetch("/api/materials/parse", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ filename: file.name, content_base64: contentBase64 }),
      });
      const data = await response.json();
      if (!response.ok) {
        throw new Error(data.detail || "Parse failed");
      }
      parsed.push(data.text);
    } catch (error) {
      failed.push(`${file.name}: ${error.message}`);
    }
  }

  if (parsed.length) {
    const existing = textarea.value.trim();
    textarea.value = [existing, ...parsed].filter(Boolean).join("\n\n---\n\n");
    syncAgentFromCard(card, index);
  }
  input.value = "";
  status.textContent = failed.length
    ? `Parsed ${parsed.length}, failed ${failed.length}`
    : `Parsed ${parsed.length} file${parsed.length === 1 ? "" : "s"}`;
  if (failed.length) {
    console.warn("Material parse errors", failed);
  }
}

function addAgent() {
  const pool = getCurrentAgentPool();
  const next = pool.length + 1;
  pool.push({
    id: `custom_${next}`,
    icon: "C",
    name: `Custom Agent ${next}`,
    role: "Describe what this agent is responsible for.",
  });
  rebuildAgentsById();
  renderAgentPicker();
}

function getAgentTools(agent) {
  if (Array.isArray(agent?.tools)) return agent.tools;
  if (agent?.id && Array.isArray(state.toolsByAgentId[agent.id])) {
    return state.toolsByAgentId[agent.id];
  }
  return state.executorTools;
}

function rebuildAgentsById() {
  const agents = [...state.agents, ...state.generatedAgents];
  state.agentsById = Object.fromEntries(agents.map((agent) => [agent.id, agent]));
}

async function startRun() {
  const userInput = elements.taskInput.value.trim();
  if (!userInput) return;

  const agentMode = getAgentMode();
  if (agentMode === "auto" && !state.generatedAgents.length) {
    elements.runButton.disabled = true;
    await generateAgentDrafts();
    elements.runButton.disabled = false;
    if (state.generatedAgents.length) {
      setStatus("idle", "Edit agents");
    }
    return;
  }

  let selectedAgents = [];
  let agentIds = [];
  const cards = [...elements.agentPicker.querySelectorAll(".agent-toggle")];
  cards.forEach((card, index) => syncAgentFromCard(card, index));
  selectedAgents = cards
    .filter((card) => card.querySelector(".agent-enabled").checked)
    .map((card) => getCurrentAgentPool()[Number(card.dataset.agentIndex)])
    .filter((agent) => agent?.id);
  agentIds = selectedAgents.map((agent) => agent.id);
  if (!agentIds.length) return;

  setStatus("running", "Starting");
  elements.runButton.disabled = true;
  elements.eventList.innerHTML = "";
  state.snapshot = null;
  renderGraph();
  renderOutputs();
  elements.planJson.textContent = "[]";

  stopPolling();
  state.renderedEventSeq = 0;

  const response = await sessionFetch("/api/runs", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      user_input: userInput,
      agent_ids: agentIds,
      agents: selectedAgents,
      agent_reference_materials: buildAgentReferencePayload(selectedAgents),
      agent_mode: agentMode,
      max_dynamic_agents: Number(elements.maxDynamicAgentsInput.value || 6),
      max_agent_rounds: Number(elements.maxAgentRoundsInput.value || 5),
      reference_materials: getReferenceMaterials(),
    }),
  });

  if (!response.ok) {
    setStatus("error", "Failed");
    elements.runButton.disabled = false;
    return;
  }

  const data = await response.json();
  state.runId = data.run_id;
  elements.runIdLabel.textContent = `Run ${state.runId}`;
  pollRun(state.runId);
}

async function generateAgentDrafts() {
  const userInput = elements.taskInput.value.trim();
  if (!userInput) return;
  setStatus("running", "Generating");
  elements.generateAgentsButton.disabled = true;
  try {
    const response = await sessionFetch("/api/agents/draft", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        user_input: userInput,
        max_dynamic_agents: Number(elements.maxDynamicAgentsInput.value || 6),
        reference_materials: getReferenceMaterials(),
      }),
    });
    if (!response.ok) {
      setStatus("error", "Failed");
      return;
    }
    const data = await response.json();
    state.generatedAgents = data.agents || [];
    rebuildAgentsById();
    renderAgentPicker();
    setStatus("idle", "Agents ready");
  } finally {
    elements.generateAgentsButton.disabled = false;
  }
}

function renderAgentMode() {
  const autoMode = getAgentMode() === "auto";
  elements.maxDynamicAgentsField.classList.toggle("hidden", !autoMode);
  elements.generateAgentsButton.classList.toggle("hidden", !autoMode);
  elements.addAgentButton.disabled = false;
  renderAgentPicker();
}

function stopPolling() {
  if (state.pollTimer) {
    clearTimeout(state.pollTimer);
    state.pollTimer = null;
  }
}

async function pollRun(runId) {
  try {
    const response = await sessionFetch(`/api/runs/${runId}`);
    if (!response.ok) throw new Error("run status request failed");
    state.snapshot = await response.json();
    (state.snapshot.events || [])
      .filter((event) => Number(event.seq || 0) > state.renderedEventSeq)
      .forEach((event) => {
        state.renderedEventSeq = Math.max(state.renderedEventSeq, Number(event.seq || 0));
        if (event.type === "agents_designed") {
          state.generatedAgents = event.payload?.agents || [];
          rebuildAgentsById();
          renderAgentPicker();
        }
        appendEvent(event);
      });
  elements.planJson.textContent = JSON.stringify(state.snapshot.plan || [], null, 2);
  renderGraph();
  renderOutputs();
    const terminal = (state.snapshot.events || []).some((event) => ["run_done", "run_stalled"].includes(event.type));
    if (terminal) {
      setStatus(state.snapshot.events.some((event) => event.type === "run_done") ? "done" : "error",
        state.snapshot.events.some((event) => event.type === "run_done") ? "Done" : "Stalled");
      elements.runButton.disabled = false;
      stopPolling();
    } else {
      state.pollTimer = setTimeout(() => pollRun(runId), 400);
    }
  } catch (error) {
    setStatus("error", "Disconnected");
    elements.runButton.disabled = false;
    stopPolling();
  }
}

function appendEvent(event) {
  const li = document.createElement("li");
  const time = new Date((event.ts || Date.now() / 1000) * 1000).toLocaleTimeString();
  li.innerHTML = `
    <strong>${escapeHtml(event.title || event.type)}</strong>
    <small>${escapeHtml(time)} · ${escapeHtml(event.type)} · ${escapeHtml(compactPayload(event.payload || {}))}</small>
  `;
  elements.eventList.appendChild(li);
  elements.eventList.scrollTop = elements.eventList.scrollHeight;
}

function compactPayload(payload) {
  const text = JSON.stringify(payload);
  return text.length > 180 ? `${text.slice(0, 180)}...` : text;
}

function renderGraph() {
  const plan = state.snapshot?.plan || [];
  const statuses = state.snapshot?.statuses || {};
  elements.graphSummary.textContent = `${plan.length} nodes`;

  const svg = elements.dagSvg;
  const width = 920;
  const height = Math.max(560, 160 + computeMaxLevel(plan) * 150);
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);

  if (!plan.length) {
    svg.innerHTML = `
      <text x="${width / 2}" y="${height / 2}" text-anchor="middle" fill="#687084" font-size="16">
        No DAG yet / 暂无 DAG
      </text>
    `;
    return;
  }

  const levels = computeLevels(plan);
  const grouped = {};
  plan.forEach((item) => {
    const level = levels[item.agent_id] ?? 0;
    grouped[level] = grouped[level] || [];
    grouped[level].push(item);
  });

  const positions = {};
  Object.entries(grouped).forEach(([levelText, items]) => {
    const level = Number(levelText);
    const y = 80 + level * 150;
    items.forEach((item, index) => {
      const x = ((index + 1) * width) / (items.length + 1);
      positions[item.agent_id] = { x, y };
    });
  });

  const marker = `
    <defs>
      <marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
        <path d="M 0 0 L 10 5 L 0 10 z" fill="#3d4655"></path>
      </marker>
    </defs>
  `;

  const edges = plan
    .flatMap((item) =>
      (item.depends_on || []).map((dep) => {
        const from = positions[dep];
        const to = positions[item.agent_id];
        if (!from || !to) return "";
        const edgeClass = statuses[dep] === "done" ? "edge" : "edge pending";
        return `<path class="${edgeClass}" d="M ${from.x} ${from.y + 42} C ${from.x} ${from.y + 88}, ${to.x} ${to.y - 88}, ${to.x} ${to.y - 42}" marker-end="url(#arrow)"></path>`;
      }),
    )
    .join("");

  const nodes = plan
    .map((item) => {
      const pos = positions[item.agent_id];
      const agent = state.agentsById[item.agent_id] || { name: item.agent_id, icon: "A" };
      const status = statuses[item.agent_id] || "pending";
      const label = `${agent.icon} ${agent.name}`.slice(0, 34);
      const deps = item.depends_on?.length ? `deps: ${item.depends_on.join(", ")}` : "deps: none";
      return `
        <g class="node ${escapeHtml(status)}" transform="translate(${pos.x - 118}, ${pos.y - 42})">
          <rect width="236" height="84"></rect>
          <text x="118" y="35" text-anchor="middle">${escapeHtml(label)}</text>
          <text class="sub" x="118" y="58" text-anchor="middle">${escapeHtml(status)} · ${escapeHtml(deps.slice(0, 28))}</text>
        </g>
      `;
    })
    .join("");

  svg.innerHTML = `${marker}${edges}${nodes}`;
}

function computeLevels(plan) {
  const ids = new Set(plan.map((item) => item.agent_id));
  const levels = Object.fromEntries(plan.map((item) => [item.agent_id, 0]));

  for (let i = 0; i < plan.length; i += 1) {
    let changed = false;
    plan.forEach((item) => {
      const deps = (item.depends_on || []).filter((dep) => ids.has(dep) && dep !== item.agent_id);
      const nextLevel = deps.length ? Math.max(...deps.map((dep) => levels[dep] ?? 0)) + 1 : 0;
      if (nextLevel > levels[item.agent_id]) {
        levels[item.agent_id] = nextLevel;
        changed = true;
      }
    });
    if (!changed) break;
  }
  return levels;
}

function computeMaxLevel(plan) {
  const levels = computeLevels(plan);
  return Math.max(0, ...Object.values(levels));
}

function renderOutputs() {
  const snapshot = state.snapshot;
  const plan = snapshot?.plan || [];
  const outputs = snapshot?.outputs || {};
  const statuses = snapshot?.statuses || {};

  if (!plan.length) {
    elements.outputsList.innerHTML = `<div class="empty-output">No outputs yet / 暂无输出</div>`;
    return;
  }

  const finalOutput = pickFinalOutput(plan, outputs);
  const finalResultHtml = finalOutput
    ? `
        <section class="final-result">
          <div class="final-result-head">
            <strong>Final Result / 最终结果</strong>
            <span>${escapeHtml(finalOutput.agentId)}</span>
          </div>
          <pre>${escapeHtml(finalOutput.output)}</pre>
        </section>
      `
    : "";

  const agentOutputsHtml = plan
    .map((item) => {
      const agent = state.agentsById[item.agent_id] || { name: item.agent_id, icon: "A" };
      const output = outputs[item.agent_id] || "";
      const status = statuses[item.agent_id] || "pending";
      return `
        <article class="output-card" data-agent-id="${escapeHtml(item.agent_id)}">
          <div class="output-card-head">
            <strong>${escapeHtml(agent.icon)} ${escapeHtml(agent.name)}</strong>
            <span class="status-pill ${escapeHtml(status)}">${escapeHtml(status)}</span>
          </div>
          <div class="output-card-body">
            <textarea>${escapeHtml(output)}</textarea>
            <div class="retry-row">
              <input type="text" placeholder="Feedback / 修改意见" />
              <button class="retry-button" ${state.runId ? "" : "disabled"}>Retry</button>
            </div>
          </div>
        </article>
      `;
    })
    .join("");

  elements.outputsList.innerHTML = agentOutputsHtml + finalResultHtml;

  elements.outputsList.querySelectorAll(".output-card").forEach((card) => {
    const agentId = card.dataset.agentId;
    card.querySelector(".retry-button").addEventListener("click", async () => {
      const editedPrior = card.querySelector("textarea").value;
      const userFeedback = card.querySelector("input").value;
      await sessionFetch(`/api/runs/${state.runId}/agents/${agentId}/retry`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ edited_prior: editedPrior, user_feedback: userFeedback }),
      });
      setStatus("running", "Retrying");
    });
  });
}

function pickFinalOutput(plan, outputs) {
  if (!plan.length) return null;
  if (outputs.writer) {
    return { agentId: "writer", output: outputs.writer };
  }

  const dependedOn = new Set();
  plan.forEach((item) => {
    (item.depends_on || []).forEach((dep) => dependedOn.add(dep));
  });
  const terminalNodes = plan.filter((item) => !dependedOn.has(item.agent_id));
  for (let i = terminalNodes.length - 1; i >= 0; i -= 1) {
    const item = terminalNodes[i];
    if (outputs[item.agent_id]) {
      return { agentId: item.agent_id, output: outputs[item.agent_id] };
    }
  }

  for (let i = plan.length - 1; i >= 0; i -= 1) {
    const item = plan[i];
    if (outputs[item.agent_id]) {
      return { agentId: item.agent_id, output: outputs[item.agent_id] };
    }
  }
  return null;
}

function setStatus(kind, label) {
  elements.connectionStatus.className = `status-pill ${kind}`;
  elements.connectionStatus.textContent = label;
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function normalizeAgentId(value) {
  return String(value || "")
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_ -]/g, "")
    .replace(/[ -]+/g, "_")
    .replace(/^_+|_+$/g, "");
}

function getAgentMode() {
  return elements.agentModeSelect.value || "manual";
}

function getCurrentAgentPool() {
  return getAgentMode() === "auto" ? state.generatedAgents : state.agents;
}

function getReferenceMaterials() {
  const text = elements.referenceInput.value.trim();
  return text ? [text] : [];
}

function getTextareaMaterials(value) {
  const text = String(value || "").trim();
  return text ? [text] : [];
}

function buildAgentReferencePayload(agents) {
  const payload = {};
  agents.forEach((agent) => {
    const refs = agent.reference_materials || [];
    if (refs.length) {
      payload[agent.id] = refs;
    }
  });
  return payload;
}

async function fileToBase64(file) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  let binary = "";
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunkSize));
  }
  return btoa(binary);
}

init().catch((error) => {
  setStatus("error", "Init failed");
  console.error(error);
});
