import argparse
import minio


def comma_separated_list(value):
    """
    Argparse type function to parse a comma-separated string into a list.
    Also handles stripping whitespace and filtering out empty strings.
    """
    return [item.strip() for item in value.split(",") if item.strip()]


def clean_up(minio_endpoint_url, minio_access_key, minio_secret_key, bucket_names):
    secure = False
    minio_endpoint = ""
    if minio_endpoint_url.find("http://") == 0:
        secure = False
        minio_endpoint = minio_endpoint_url.lstrip("http://")
    elif minio_endpoint_url.find("https://") == 0:
        secure = True
        minio_endpoint = minio_endpoint_url.lstrip("https://")

    client = minio.Minio(
        minio_endpoint,
        access_key=minio_access_key,
        secret_key=minio_secret_key,
        secure=secure,
    )

    for bucket_name in bucket_names:
        # remove rocksdb data in bucket
        if not client.bucket_exists(bucket_name):
            print(f"minio cleanup: bucket {bucket_name} not exists")
            continue
        del_objects = [minio.deleteobjects.DeleteObject(x.object_name)
                       for x in client.list_objects(bucket_name,
                                                    recursive=True)]
        errors = client.remove_objects(bucket_name, del_objects)
        for e in errors:
            raise e

        client.remove_bucket(bucket_name)
        print(f"rocksdbcloud bucket#{bucket_name} has been cleaned up.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--minio_endpoint", required=True, help="Endpoint of minio s3.")
    parser.add_argument(
        "--minio_access_key", required=True, help="AccessKey of minio s3."
    )
    parser.add_argument(
        "--minio_secret_key", required=True, help="SecretKey of minio s3."
    )
    parser.add_argument(
        "--bucket_names",
        required=True,
        type=comma_separated_list,
        default=[],
        help="Comma-separated name(s) of bucket(s) to clean up.",
    )

    parsed_args = parser.parse_args()

    clean_up(
        minio_endpoint_url=parsed_args.minio_endpoint,
        minio_access_key=parsed_args.minio_access_key,
        minio_secret_key=parsed_args.minio_secret_key,
        bucket_names=parsed_args.bucket_names,
    )
