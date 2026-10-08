"use strict";

const METHODS = ["get", "post", "put", "patch", "delete"];
const byId = (id) => document.getElementById(id);

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function operationCard(method, path, operation) {
  const card = element("article", "api-operation");
  const heading = element("div", "api-operation-heading");
  heading.append(
    element("span", `api-method ${method}`, method.toUpperCase()),
    element("code", "api-path", path)
  );
  card.append(heading, element("h3", "", operation.summary || operation.operationId || path));
  if (operation.description) card.append(element("p", "", operation.description));

  const facts = element("dl", "api-facts");
  const parameters = Array.isArray(operation.parameters) ? operation.parameters : [];
  if (parameters.length) {
    const value = parameters.map((parameter) => {
      const required = parameter.required ? "required" : "optional";
      return `${parameter.name} (${parameter.in}, ${required})`;
    }).join(" · ");
    facts.append(element("dt", "", "Parameters"), element("dd", "", value));
  }
  const content = operation.requestBody?.content;
  if (content) {
    facts.append(
      element("dt", "", "Request"),
      element("dd", "", Object.keys(content).join(", "))
    );
  }
  const responses = operation.responses || {};
  const responseText = Object.entries(responses)
    .map(([status, response]) => `${status} ${response.description || ""}`.trim())
    .join(" · ");
  if (responseText) {
    facts.append(element("dt", "", "Responses"), element("dd", "", responseText));
  }
  if (facts.children.length) card.append(facts);
  return card;
}

function render(specification) {
  const groups = new Map();
  for (const tag of specification.tags || []) {
    groups.set(tag.name, { description: tag.description || "", operations: [] });
  }
  for (const [path, item] of Object.entries(specification.paths || {})) {
    for (const method of METHODS) {
      const operation = item[method];
      if (!operation) continue;
      const tag = operation.tags?.[0] || "Other";
      if (!groups.has(tag)) groups.set(tag, { description: "", operations: [] });
      groups.get(tag).operations.push({ method, path, operation });
    }
  }

  const target = byId("api-groups");
  target.replaceChildren();
  for (const [name, group] of groups) {
    if (!group.operations.length) continue;
    const section = element("section", "api-group");
    section.append(element("h2", "", name));
    if (group.description) section.append(element("p", "api-group-description", group.description));
    for (const entry of group.operations) {
      section.append(operationCard(entry.method, entry.path, entry.operation));
    }
    target.append(section);
  }
  byId("api-doc-status").hidden = true;
}

Promise.all([
  fetch("/openapi.json", { cache: "no-store" }).then((response) => {
    if (!response.ok) throw new Error(`OpenAPI HTTP ${response.status}`);
    return response.json();
  }),
  fetch("/health", { cache: "no-store" })
    .then((response) => response.ok ? response.json() : null)
    .catch(() => null),
]).then(([specification, health]) => {
  byId("runtime-version").textContent = health?.version || specification.info.version;
  render(specification);
}).catch((error) => {
  byId("api-doc-status").className = "api-doc-status error";
  byId("api-doc-status").textContent = `Could not load API documentation: ${error.message}`;
});
