/**
 * Supabase の `protein_logs` を毎時間チェックして、
 * "今日(JST)に作成されたログ" が1件でもあれば Notion DB の
 * 「日付」=今日 のページの「🥛プロテイン」チェックを ON にする GAS。
 *
 * 1) Apps Script の「プロジェクトの設定」→「スクリプト プロパティ」に以下を設定:
 *    - SUPABASE_REST_URL      : https://PROJECT.supabase.co/rest/v1/protein_logs
 *    - SUPABASE_API_KEY       : Supabase の anon key など
 *    - NOTION_TOKEN           : Notion integration token (secret_...)
 *    - NOTION_DATABASE_ID     : 対象DBのID
 *    - NOTION_DATE_PROP       : (任意) 既定="日付"
 *    - NOTION_PROTEIN_PROP    : (任意) 既定="🥛プロテイン"
 *
 * 2) `setupHourlyTrigger()` を1回実行してトリガーを作成。
 */

const TZ = "Asia/Tokyo";
const DEFAULT_NOTION_DATE_PROP = "日付";
const DEFAULT_NOTION_PROTEIN_PROP = "🥛プロテイン";

function setupHourlyTrigger() {
  const handler = "checkProteinLogAndUpdateNotion";
  ScriptApp.getProjectTriggers().forEach((t) => {
    if (t.getHandlerFunction() === handler) ScriptApp.deleteTrigger(t);
  });
  ScriptApp.newTrigger(handler).timeBased().everyHours(1).create();
}

function checkProteinLogAndUpdateNotion() {
  const cfg = getConfig_();

  const { todayJst, startIsoJst, endIsoJst } = getTodayRangeJst_();
  Logger.log(`[JST] today=${todayJst} range=${startIsoJst}..${endIsoJst}`);

  const hasLog = hasTodayProteinLog_(cfg.supabaseRestUrl, cfg.supabaseApiKey, startIsoJst, endIsoJst);
  if (!hasLog) {
    Logger.log("[Supabase] no logs today -> skip");
    return;
  }

  const pageIds = findNotionPagesByDate_(cfg.notionToken, cfg.notionDatabaseId, cfg.notionDateProp, todayJst);
  if (pageIds.length === 0) {
    Logger.log("[Notion] no page for today -> skip");
    return;
  }

  pageIds.forEach((pageId) => {
    updateNotionCheckbox_(cfg.notionToken, pageId, cfg.notionProteinProp, true);
  });
  Logger.log(`[Notion] updated ${pageIds.length} page(s)`);
}

function getConfig_() {
  const props = PropertiesService.getScriptProperties();
  const supabaseRestUrl = mustGetProp_(props, "SUPABASE_REST_URL");
  const supabaseApiKey = mustGetProp_(props, "SUPABASE_API_KEY");
  const notionToken = mustGetProp_(props, "NOTION_TOKEN");
  const notionDatabaseId = mustGetProp_(props, "NOTION_DATABASE_ID");
  const notionDateProp = props.getProperty("NOTION_DATE_PROP") || DEFAULT_NOTION_DATE_PROP;
  const notionProteinProp = props.getProperty("NOTION_PROTEIN_PROP") || DEFAULT_NOTION_PROTEIN_PROP;

  return {
    supabaseRestUrl,
    supabaseApiKey,
    notionToken,
    notionDatabaseId,
    notionDateProp,
    notionProteinProp,
  };
}

function mustGetProp_(props, key) {
  const value = props.getProperty(key);
  if (!value) throw new Error(`Missing Script Property: ${key}`);
  return value;
}

function getTodayRangeJst_() {
  const now = new Date();
  const todayJst = Utilities.formatDate(now, TZ, "yyyy-MM-dd");

  const startIsoJst = `${todayJst}T00:00:00+09:00`;
  const start = new Date(startIsoJst);
  const tomorrowJst = Utilities.formatDate(new Date(start.getTime() + 24 * 60 * 60 * 1000), TZ, "yyyy-MM-dd");
  const endIsoJst = `${tomorrowJst}T00:00:00+09:00`;

  return { todayJst, startIsoJst, endIsoJst };
}

function hasTodayProteinLog_(supabaseRestUrl, supabaseApiKey, startIsoJst, endIsoJst) {
  const url =
    supabaseRestUrl +
    "?" +
    [
      "select=id",
      `created_at=gte.${encodeURIComponent(startIsoJst)}`,
      `created_at=lt.${encodeURIComponent(endIsoJst)}`,
      "order=created_at.desc",
      "limit=1",
    ].join("&");

  const res = UrlFetchApp.fetch(url, {
    method: "get",
    headers: {
      apikey: supabaseApiKey,
      Authorization: `Bearer ${supabaseApiKey}`,
    },
    muteHttpExceptions: true,
  });

  const code = res.getResponseCode();
  const text = res.getContentText();
  if (code < 200 || code >= 300) throw new Error(`[Supabase] ${code}: ${text}`);

  const rows = text ? JSON.parse(text) : [];
  return Array.isArray(rows) && rows.length > 0;
}

function findNotionPagesByDate_(notionToken, databaseId, datePropName, dateStr) {
  const url = `https://api.notion.com/v1/databases/${databaseId}/query`;
  const payload = {
    filter: {
      property: datePropName,
      date: { equals: dateStr }, // yyyy-mm-dd
    },
  };

  const res = UrlFetchApp.fetch(url, {
    method: "post",
    contentType: "application/json",
    headers: notionHeaders_(notionToken),
    payload: JSON.stringify(payload),
    muteHttpExceptions: true,
  });

  const code = res.getResponseCode();
  const text = res.getContentText();
  if (code < 200 || code >= 300) throw new Error(`[Notion] query ${code}: ${text}`);

  const data = text ? JSON.parse(text) : {};
  const results = Array.isArray(data.results) ? data.results : [];
  return results.map((p) => p.id).filter(Boolean);
}

function updateNotionCheckbox_(notionToken, pageId, checkboxPropName, checked) {
  const url = `https://api.notion.com/v1/pages/${pageId}`;
  const payload = {
    properties: {
      [checkboxPropName]: { checkbox: !!checked },
    },
  };

  const res = UrlFetchApp.fetch(url, {
    method: "patch",
    contentType: "application/json",
    headers: notionHeaders_(notionToken),
    payload: JSON.stringify(payload),
    muteHttpExceptions: true,
  });

  const code = res.getResponseCode();
  const text = res.getContentText();
  if (code < 200 || code >= 300) throw new Error(`[Notion] update ${code}: ${text}`);
}

function notionHeaders_(token) {
  return {
    Authorization: `Bearer ${token}`,
    "Notion-Version": "2022-06-28",
  };
}

