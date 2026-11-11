Build vision applications with AI
================================================================================

:link_to_translation:`zh_CN:[中文]`

Describe an idea to your AI assistant and develop it into a Python script running on an ESP-VISION board. The assistant can find examples, prepare firmware and models, deploy the application, inspect camera images and logs, and improve the result with you.

Connect your AI assistant
--------------------------------------------------------------------------------

The ESP-VISION plugin connects your assistant to official knowledge and local device tools. Choose the setup path for your client in the `official getting-started guide <https://vision.espressif.com/#workflow>`_.

WorkBuddy and QoderWork
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Install through the application marketplace: search for ``esp-vision`` and select the listing below. These are the listing titles shown by the official website.

.. list-table:: Marketplace listings
   :header-rows: 1
   :widths: 25 75

   * - Client
     - Listing
   * - WorkBuddy
     - 嵌入式视觉开发专家 | ESP-Vision AI视觉
   * - QoderWork
     - ESP-Vision 嵌入式 AI 视觉开发

Codex and Claude Code
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For Codex, start with:

.. note::

   Help me use ESP-VISION in Codex. Official installation guide: https://www.npmjs.com/package/@esp-vision/plugin

For Claude Code:

.. note::

   Help me use ESP-VISION in Claude Code. Official installation guide: https://www.npmjs.com/package/@esp-vision/plugin

Cursor and other assistants
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For Cursor and other clients, see the `plugin installation guide <https://github.com/espressif/esp-vision/blob/master/host/esp-vision-plugin/README.md>`_.

.. note::

   I want to use ESP-VISION in my current AI assistant. Official setup guide: https://github.com/espressif/esp-vision/blob/master/host/esp-vision-plugin/README.md

After setup, start a new conversation in your project. For USB development, the AI assistant needs access to the computer connected to the board.

Connect a board and run an application
--------------------------------------------------------------------------------

Connect a :doc:`board supported by ESP-VISION <../target-support/index>` to your computer with a USB data cable. For first use, the assistant can download and flash official firmware to prepare the board for vision applications:

.. note::

   This is a new board supported by ESP-VISION. Install ESP-VISION firmware so I can take pictures with its camera. There is no data on the board that I need to keep.

After the initial firmware installation, start developing an application. A board that already has compatible firmware can begin with the application request; a device with networking configured can also connect over the local network.

.. note::

   Show live camera video on the board's screen with a frame counter, and leave it running for me to see.

Once running, the screen shows a continuously updating camera view and frame counter. You also receive the Python script to adapt and reuse.

.. blockdiag::

   blockdiag {
     orientation = portrait;
     node_width = 200;
     A [label = "Describe the goal"];
     B [label = "Prepare the application"];
     C [label = "Run on the board"];
     D [label = "Inspect images and logs"];
     E [label = "Improve and verify"];
     A -> B -> C -> D -> E;
     E -> C;
   }

Everyday tasks
--------------------------------------------------------------------------------

Start with the feature, use case or problem you have in mind. The plugin's workflow handles implementation, while you can continue shaping the result.

Color tracking
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::

   Track red objects in the camera view and draw a box around them in real time. The box should follow the object as it moves.

Move a red object in front of the camera and see how well the box follows it. You can add requirements for a different color, object or movement speed.

Update firmware
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::

   Update the board to the latest compatible official ESP-VISION firmware, keeping my scripts and settings.

After the update, you receive the installed version and the device's running results.

Learn about the connected device
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::

   What model is the board connected to my computer, what firmware does it have, and which vision features does it support?

Device information helps you choose an application direction, such as image processing, object detection or a camera display.

Face detection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::

   Have the board detect faces and draw boxes around them on screen in real time, with the detection frame rate displayed.

Observe what happens when a face enters, moves through and leaves the camera view, and use the displayed frame rate to assess responsiveness.

QR-code recognition
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::

   I want to read QR codes with the camera. Outline each code on screen, show its content and keep updating as I move it.

Add requirements for your use case, such as reading smaller QR codes or making their contents easier to read.

Develop over Wi-Fi
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::

   Connect my board to Wi-Fi so I can update its program and view the camera over a wireless connection.

For initial setup, connect a Wi-Fi-capable board over USB and provide network details when the assistant asks. Once configured, continue development over the LAN; the board reconnects to its saved network after power-on. Alternatively, open the browser flashing panel on the `website <https://vision.espressif.com/>`_, choose “Configure Wi-Fi Only” under “Flash Mode”, connect the serial port, and enter and save your network settings. See :doc:`../communication/host-integration` for the complete setup procedure and connection troubleshooting.

USB camera
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::

   Turn the board into a USB camera so my computer can show live video with boxes around detected faces.

Use a board with USB-OTG Device support and connect its USB-OTG port. Select ``ESP-VISION Camera`` in your computer's camera application to view the result. See :doc:`../api-reference/uvc` for supported image formats, resolution and APIs.

Investigate and fix a problem
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::

   The board's screen goes black after the camera program starts. Please fix it.

Share what you observed, along with any available error message or image. You can also describe how the application behaved before the problem appeared.

Improve the result through conversation
--------------------------------------------------------------------------------

Keep working in the same conversation so the assistant can use the project, device and previous results as context. For the tracking application, you could follow up with:

.. note::

   The red-object tracking box flickers in dim lighting. I want the tracking to stay stable in low light.

For the detection application:

.. note::

   The status text is too small. Make it larger and add the detection frame rate.

Describe the next goal in terms of your experience: steadier tracking, faster response, clearer images or more readable text.

Tools behind the conversation
--------------------------------------------------------------------------------

The host-side tools connect natural-language development to official knowledge and real hardware:

.. list-table:: ESP-VISION host tools
   :header-rows: 1
   :widths: 30 70

   * - Tool
     - Role
   * - Plugin (``@esp-vision/plugin``)
     - Gives the AI client the ESP-VISION Skill, bundled Device CLI and official knowledge connection.
   * - Device CLI (``@esp-vision/device-cli``)
     - Lets the assistant discover devices, manage firmware and files, run Python scripts and capture images on the local host.
   * - Knowledge MCP
     - Supplies official API documentation, board capabilities and examples for the assistant to consult.

For further detail, see the :doc:`Device CLI reference <../device-core/cli>`, :doc:`Python script guidance <agent-code-generation>` and :doc:`host SDK guide <../device-core/index>`.
