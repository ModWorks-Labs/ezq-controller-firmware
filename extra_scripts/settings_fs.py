import csv
from os.path import join

Import("env")


def project_option(name: str) -> str:
    return env.GetProjectOption(name)


def partition_size_hex(label: str) -> str:
    partitions_csv = env.subst("$PARTITIONS_TABLE_CSV")
    with open(partitions_csv, newline="", encoding="utf-8") as fp:
        reader = csv.reader(fp)
        for row in reader:
            if not row or row[0].strip().startswith("#"):
                continue
            columns = [col.strip() for col in row]
            if len(columns) < 5:
                continue
            if columns[0] == label:
                return columns[4]
    raise ValueError(f"Partition '{label}' not found in {partitions_csv}")


def add_fs_target(
    target_name: str,
    title: str,
    folder: str,
    partition_option: str,
) -> None:
    host = project_option("custom_ota_host")
    port = project_option("custom_ota_port")
    partition = project_option(partition_option)
    size_hex = partition_size_hex(partition)
    image_name = f"{partition}.bin"

    image = env.Command(
        join("$BUILD_DIR", image_name),
        None,
        env.VerboseAction(
            " ".join(
                [
                    '"$MKFSTOOL"',
                    "-c",
                    f'"$PROJECT_DIR/{folder}"',
                    "-s",
                    size_hex,
                    "-p",
                    "256",
                    "-b",
                    "4096",
                    '"$TARGET"',
                ]
            ),
            f"Building {title.lower()} image from '$PROJECT_DIR/{folder}' to $TARGET",
        ),
    )
    env.NoCache(image)
    AlwaysBuild(image)

    upload_action = env.VerboseAction(
        (
            'python "$PROJECT_DIR/utils/ota_upload.py" '
            f'--host {host} --port {port} '
            f'--filesystem-partition {partition} '
            '--image $SOURCE'
        ),
        "Uploading $SOURCE",
    )

    env.AddCustomTarget(
        f"build{target_name}",
        image,
        image,
        f"Build {title}",
        f"Build the SPIFFS image for the {partition} partition",
    )

    env.AddCustomTarget(
        f"upload{target_name}",
        image,
        [upload_action],
        f"Upload {title}",
        f"Upload the SPIFFS image for the {partition} partition over OTA",
    )


add_fs_target("settingsfs", "Settings Filesystem Image", "settings", "custom_settings_partition")
add_fs_target("webuifs", "Web UI Filesystem Image", "web_ui", "custom_web_ui_partition")
