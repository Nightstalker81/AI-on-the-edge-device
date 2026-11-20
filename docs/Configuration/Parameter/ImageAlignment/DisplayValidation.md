# Display Validation

|                | WebUI | REST API |
|:---             |:---   |:---      |
| Configuration   | Settings → Display Validation | `imagealignment.displayvalidation` |

## Description

Display validation checks selected areas of the reference image after alignment and before digit recognition. Add one or more regions around stable features that identify the desired display page, such as unit labels. Every configured region must meet the similarity threshold for recognition to continue.

The check uses a grayscale template comparison and tolerates a position offset of up to two pixels. It does not interpret text or meter values. The default threshold is 95%; calibrate it using several images of both valid and invalid pages. Lower it if lighting changes cause valid pages to be rejected, or raise it if other display pages are incorrectly accepted. Similarity is an average over the entire region, so a large background area can hide a missing feature.

The similarity of each checked region is written at debug log level. A rejected region and its similarity are logged at warning level.

When validation is enabled, at least one region must be configured. A failed check aborts the current processing cycle before the CNN/OCR steps and skips publishing measurement data. The reference image must show the desired display page; replacing it changes the templates used after the configuration is reloaded.

## Configuration

```json
{
  "imagealignment": {
    "displayvalidation": {
      "enabled": true,
      "similaritythreshold": 95,
      "roi": [
        { "x": 400, "y": 120, "dx": 35, "dy": 18 },
        { "x": 450, "y": 145, "dx": 40, "dy": 18 }
      ]
    }
  }
}
```

Coordinates and dimensions are in pixels in the reference image. Each region is a template of a fixed display feature; avoid including digits or other parts of the display that can change during normal operation.
