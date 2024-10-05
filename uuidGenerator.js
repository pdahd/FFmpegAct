// Cloudflare Workers Script

async function handleRequest(request) {
  return new Response(
    `<!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8" />
      <meta name="viewport" content="width=device-width, initial-scale=1.0" />
      <title>UUID Generator</title>
      <style>
        body {
          font-family: Arial, sans-serif;
          background-color: #333333; /* Adobe风格深灰 */
          color: #FFFFFF;
          margin: 0;
          padding: 0;
          display: flex;
          flex-direction: column;
          align-items: center;
          justify-content: center;
          height: 100vh;
          overflow: hidden; /* 禁止溢出 */
        }

        .container {
          background-color: #444444; /* Adobe风格浅灰 */
          padding: 10px;
          border-radius: 10px;
          box-shadow: 0px 4px 12px rgba(0, 0, 0, 0.5);
          width: 90%; /* 调整宽度 */
          max-width: 350px; /* 最大宽度 */
          text-align: center;
          overflow-y: auto; /* 允许竖向滚动 */
          max-height: 90%; /* 最大高度 */
        }

        h1 {
          margin: 0;
          font-size: 1.5rem; /* 调整标题大小 */
        }

        input[type="number"] {
          width: calc(100% - 20px);
          padding: 10px;
          border: none;
          border-radius: 5px;
          margin: 10px 0;
          font-size: 16px;
        }

        button {
          width: 100%;
          padding: 10px;
          background-color: #0078D7; /* Adobe风格的蓝色按钮 */
          border: none;
          border-radius: 5px;
          color: #FFFFFF;
          font-size: 16px;
          cursor: pointer;
        }

        button:hover {
          background-color: #005A9E;
        }

        table {
          width: 100%;
          margin-top: 10px;
          border-collapse: collapse;
        }

        table, th, td {
          border: 1px solid #D2B48C; /* 卡其色 */
        }

        th, td {
          padding: 10px;
          text-align: left;
          position: relative; /* 为复制按钮设置位置 */
        }

        .copy-icon {
          width: 20px;
          height: 20px;
          position: absolute;
          right: 10px;
          top: 50%;
          transform: translateY(-50%);
          cursor: pointer;
        }

      </style>
    </head>
    <body>
      <div class="container">
        <h1>UUID Generator</h1>
        <input type="number" id="uuidCount" placeholder="Enter number of UUIDs" />
        <button onclick="generateUUIDs()">Generate</button>
        <table id="uuidTable">
          <thead>
            <tr>
              <th>#</th>
              <th>UUID</th>
            </tr>
          </thead>
          <tbody>
          </tbody>
        </table>
      </div>

      <script>
        let selectedUUID = "";

        function generateUUID() {
          return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c) {
            var r = Math.random() * 16 | 0, v = c === 'x' ? r : (r & 0x3 | 0x8);
            return v.toString(16);
          });
        }

        function generateUUIDs() {
          const count = document.getElementById('uuidCount').value;
          const tableBody = document.getElementById('uuidTable').getElementsByTagName('tbody')[0];
          tableBody.innerHTML = ''; // 清空之前的UUID

          for (let i = 1; i <= count; i++) {
            const uuid = generateUUID();
            const row = tableBody.insertRow();
            const cell1 = row.insertCell(0);
            const cell2 = row.insertCell(1);
            cell1.textContent = i;
            cell2.textContent = uuid;
            cell2.style.cursor = "pointer";

            // 添加复制图标
            const copyIcon = document.createElement('img');
            copyIcon.src = 'https://img.icons8.com/material-outlined/24/ffffff/copy.png'; // 复制图标链接
            copyIcon.className = 'copy-icon';
            copyIcon.onclick = function() {
              copyUUID(uuid);
            };
            cell2.appendChild(copyIcon);
          }
        }

        function copyUUID(uuid) {
          const el = document.createElement('textarea');
          el.value = uuid;
          document.body.appendChild(el);
          el.select();
          document.execCommand('copy');
          document.body.removeChild(el);
          alert('UUID copied to clipboard: ' + uuid);
        }
      </script>
    </body>
    </html>`,
    {
      headers: {
        'content-type': 'text/html;charset=UTF-8',
      },
    },
  );
}

addEventListener('fetch', event => {
  event.respondWith(handleRequest(event.request));
});
